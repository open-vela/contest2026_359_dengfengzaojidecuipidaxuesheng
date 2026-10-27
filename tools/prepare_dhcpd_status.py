#!/usr/bin/env python3
"""Add coherent listener readiness to the pinned NuttX DHCP daemon."""

import hashlib
from pathlib import Path


ORIGINAL_SHA256 = '502eb2f753d3d8688b3e0fb21c5d8f4f5ea6779fa29e1cd89aa32accf3612812'
LEGACY_PATCHED_SHA256 = '5ad1b92cc466615db843312b7ced631213cefc805a94c1d5a840fc1ba072b631'
PATCHED_SHA256 = '1a13b3e06e740e978675a073d1131d5dd7ffe6195a2fec9e4b94f38893a48cda'


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Unknown DHCP runtime anchor: ' + old[:100])
    return text.replace(old, new, 1)


START = r'''int dhcpd_start(FAR const char *interface)
{
  FAR char *argv[2];
  int ret;
  int pid;

  if (interface == NULL || strnlen(interface, IFNAMSIZ) == 0 ||
      strnlen(interface, IFNAMSIZ) >= IFNAMSIZ)
    {
      return -EINVAL;
    }

  ret = pthread_mutex_lock(&g_dhcpd_control);
  if (ret != 0) return -ret;
  pthread_mutex_lock(&g_dhcpd_status_lock);
  if (g_dhcpd_child >= 0 || g_dhcpd_status.running || g_dhcpd_starting)
    {
      pthread_mutex_unlock(&g_dhcpd_status_lock);
      pthread_mutex_unlock(&g_dhcpd_control);
      return -EBUSY;
    }

  g_dhcpd_starting = true;
  g_dhcpd_can_signal = false;
  g_dhcpd_status.ready = false;
  g_dhcpd_status.last_error = 0;
  g_dhcpd_status.pid = -1;
  pthread_mutex_unlock(&g_dhcpd_status_lock);
  argv[0] = (char *)interface;
  argv[1] = NULL;
  pid = task_create("dhcpd", CONFIG_NETUTILS_DHCPD_PRIORITY,
                    CONFIG_NETUTILS_DHCPD_STACKSIZE, dhcpd_task_run, argv);
  if (pid < 0)
    {
      ret = -errno;
      dhcpd_runtime_exited(ret);
      pthread_mutex_unlock(&g_dhcpd_control);
      return ret;
    }

  g_dhcpd_child = pid;
  pthread_mutex_lock(&g_dhcpd_status_lock);
  g_dhcpd_status.pid = pid;
  /* Stop may signal only after the child's handler has been installed.
   * This handshake does not assert that bind or DHCP succeeded.
   */

  while (g_dhcpd_starting && !g_dhcpd_can_signal)
    {
      ret = pthread_cond_wait(&g_dhcpd_status_cond, &g_dhcpd_status_lock);
      if (ret != 0) break;
    }

  ret = ret != 0 ? -ret : g_dhcpd_status.last_error;
  pthread_mutex_unlock(&g_dhcpd_status_lock);
  pthread_mutex_unlock(&g_dhcpd_control);
  return ret;
}'''


STOP = r'''int dhcpd_stop(void)
{
  bool signal_child;
  pid_t waited;
  pid_t pid;
  int ret = pthread_mutex_lock(&g_dhcpd_control);

  if (ret != 0) return -ret;
  pid = g_dhcpd_child;
  if (pid < 0)
    {
      pthread_mutex_lock(&g_dhcpd_status_lock);
      ret = g_dhcpd_status.running || g_dhcpd_starting ? -EBUSY : OK;
      pthread_mutex_unlock(&g_dhcpd_status_lock);
      pthread_mutex_unlock(&g_dhcpd_control);
      return ret;
    }

  pthread_mutex_lock(&g_dhcpd_status_lock);
  signal_child = g_dhcpd_status.running && g_dhcpd_can_signal;
  pthread_mutex_unlock(&g_dhcpd_status_lock);
  if (signal_child && kill(pid, CONFIG_NETUTILS_DHCPD_SIGWAKEUP) < 0 &&
      errno != ESRCH)
    {
      ret = -errno;
      dhcpd_runtime_listener(false, ret);
      pthread_mutex_unlock(&g_dhcpd_control);
      return ret;
    }

  do
    {
      waited = waitpid(pid, NULL, 0);
    }
  while (waited < 0 && errno == EINTR);
  if (waited != pid)
    {
      ret = waited < 0 ? -errno : -ECHILD;
      dhcpd_runtime_listener(false, ret);
      pthread_mutex_unlock(&g_dhcpd_control);
      return ret;
    }

  g_dhcpd_child = -1;
  pthread_mutex_lock(&g_dhcpd_status_lock);
  g_dhcpd_status.pid = -1;
  g_dhcpd_status.running = false;
  g_dhcpd_status.ready = false;
  pthread_mutex_unlock(&g_dhcpd_status_lock);
  pthread_mutex_unlock(&g_dhcpd_control);
  return OK;
}'''


def atomic_options(text):
    for field in ('routerip', 'dnsip'):
        old = '  in_addr_t ds_' + field + ';'
        new = '  atomic_uint_least32_t ds_' + field + ';'
        if new not in text:
            text = once(text, old, new)
    return text


def adapt(text):
    text = once(text, '#include <stdbool.h>',
                '#include <stdbool.h>\n#include <stdatomic.h>')
    text = once(text, '#include <sys/socket.h>',
                '#include <sys/socket.h>\n#include <sys/time.h>')
    text = once(text, '  uint8_t                   ds_state;',
                '  atomic_int                ds_state;')
    # The server has one shared lease table; status/control metadata stays
    # beside that singleton rather than in a caller's short-lived task.
    text = once(text, 'static struct dhcpd_config_s g_dhcpd_config =',
                '#include "dhcpd_runtime.inc"\n\n'
                'static struct dhcpd_config_s g_dhcpd_config =')
    # Preserve socket/setsockopt/ioctl/bind order and propagate errno before
    # close can overwrite it. No DHCP packet, routing, or lease logic changes.
    for begin, end in [('static inline int dhcpd_socket(',
                        ' * Name: dhcpd_initpacket'),
                       ('static inline int dhcpd_openlistener(',
                        ' * Name: dhcpd_task_run')]:
        a = text.index(begin)
        b = text.index(end, a)
        body = text[a:b]
        body = body.replace('      close(sockfd);\n      return ERROR;',
                            '      int error = errno;\n'
                            '      close(sockfd);\n      return -error;')
        body = body.replace('      return ERROR;', '      return -errno;')
        if begin.endswith('dhcpd_openlistener('):
            body = once(body,
                        'nerr("ERROR: socket failed: %d\\n", errno);\n'
                        '      return -errno;',
                        'nerr("ERROR: socket failed: %d\\n", -sockfd);\n'
                        '      return sockfd;')
            body = once(body, '  struct sockaddr_in addr;',
                        '  struct sockaddr_in addr;\n'
                        '  struct timeval timeout = {1, 0};')
            body = once(body, '  /* Get the IP address of the selected device */',
                        '''  /* A signal can arrive just before recv starts. A bounded receive
   * timeout closes that lost-wakeup window without changing packet flow.
   */

  if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout)) < 0)
    {
      int error = errno;
      close(sockfd);
      return -error;
    }

  /* Get the IP address of the selected device */''')
        text = text[:a] + body + text[b:]
    text = once(text, '#ifndef CONFIG_BUILD_KERNEL\nstatic int dhcpd_task_run',
                'static int dhcpd_run_internal(FAR const char *interface,\n'
                '                              bool managed);\n\n'
                '#ifndef CONFIG_BUILD_KERNEL\nstatic int dhcpd_task_run')
    text = once(text, '  return dhcpd_run(argv[1]);',
                '  return dhcpd_run_internal(argv[1], true);')
    # The original PID lookup races child startup and loses exited children
    # before waitpid. The serialized lifecycle retains the actual child PID.
    a = text.index('#if !defined(CONFIG_BUILD_KERNEL) || defined(CONFIG_SCHED_WAITPID)\nstatic pid_t dhcpd_get_pid')
    b = text.index('#ifndef CONFIG_DISABLE_SIGNALS', a)
    text = text[:a] + text[b:]
    text = once(text, 'int dhcpd_run(FAR const char *interface)',
                'static int dhcpd_run_internal(FAR const char *interface,\n'
                '                              bool managed)')
    text = once(text, '  int sockfd = -1;\n  int nbytes;',
                '  int sockfd = -1;\n  int nbytes;\n  int result;')
    a = text.index('  /* Exit if the dhcpd is already executed */')
    b = text.index('  /* Initialize everything to zero */', a)
    text = text[:a] + '''  result = dhcpd_runtime_enter(managed);
  if (result < 0) return result;

''' + text[b:]
    text = once(text, '      return -ENOMEM;',
                '      dhcpd_runtime_exited(-ENOMEM);\n      return -ENOMEM;')
    text = once(text,
                '  sigaction(CONFIG_NETUTILS_DHCPD_SIGWAKEUP, &act, NULL);',
                '''  if (sigaction(CONFIG_NETUTILS_DHCPD_SIGWAKEUP, &act, NULL) < 0)
    {
      result = -errno;
      goto finished;
    }''')
    text = once(text, '  g_dhcpd_daemon.ds_state = DHCPD_RUNNING;',
                '  dhcpd_runtime_started();\n  result = OK;')
    text = once(text,
                '                nerr("ERROR: Failed to create socket\\n");\n'
                '                break;',
                '                result = sockfd;\n'
                '                dhcpd_runtime_listener(false, result);\n'
                '                nerr("ERROR: Failed to create socket\\n");\n'
                '                break;')
    text = once(text, '''                break;
            }
        }

      /* Read the next g_state.ds_outpacket */''',
                '''                break;
            }

          dhcpd_runtime_listener(true, 0);
        }

      if (g_dhcpd_daemon.ds_state == DHCPD_STOP_REQUESTED) break;

      /* Read the next g_state.ds_outpacket */''')
    text = once(text, '''          if (errno != EINTR && errno != EAGAIN)
            {
              close(sockfd);''',
                '''          if (errno != EINTR && errno != EAGAIN)
            {
              dhcpd_runtime_listener(false, -errno);
              close(sockfd);''')
    text = once(text, '  free(g_dhcpd_daemon.ds_data);',
                '''finished:
  dhcpd_runtime_listener(false, result);
  if (sockfd >= 0) close(sockfd);
  free(g_dhcpd_daemon.ds_data);''')
    text = once(text, '''  g_dhcpd_daemon.ds_state = DHCPD_STOPPED;

  return OK;
}''', '''  g_dhcpd_daemon.ds_state = DHCPD_STOPPED;
  dhcpd_runtime_exited(result);
  return result;
}

int dhcpd_run(FAR const char *interface)
{
  return dhcpd_run_internal(interface, false);
}''')
    for name, replacement in [('dhcpd_start', START), ('dhcpd_stop', STOP)]:
        a = text.index('int ' + name + '(')
        b = text.index('\n}\n', a) + 2
        text = text[:a] + replacement + text[b:]
    return atomic_options(text)


def apply(apps):
    apps = Path(apps)
    path = apps / 'netutils/dhcpd/dhcpd.c'
    original = path.read_bytes()
    digest = hashlib.sha256(original).hexdigest()
    if digest == ORIGINAL_SHA256:
        updated = adapt(original.decode()).encode()
    elif digest == LEGACY_PATCHED_SHA256:
        updated = atomic_options(original.decode()).encode()
    elif PATCHED_SHA256 and digest == PATCHED_SHA256:
        updated = original
    else:
        raise ValueError('Unknown DHCPD source; refusing runtime patch: ' + digest)
    if PATCHED_SHA256 and hashlib.sha256(updated).hexdigest() != PATCHED_SHA256:
        raise ValueError('Unexpected DHCP runtime transformation')
    overlay = Path(__file__).resolve().parents[1] / 'app/espdl-quickapp/overlay/apps'
    copies = ['include/netutils/dhcpd_runtime.h',
              'netutils/dhcpd/dhcpd_runtime.inc']
    for name in copies:
        target = apps / name
        target.parent.mkdir(parents=True, exist_ok=True)
        content = (overlay / name).read_bytes()
        if not target.exists() or target.read_bytes() != content:
            target.write_bytes(content)
    if updated != original:
        path.write_bytes(updated)
    return {'ready_means': 'UDP server port 67 bind completed',
            'start_stop': 'serialized parent-owner task lifecycle',
            'source_sha256': hashlib.sha256(updated).hexdigest(),
            'support_sha256': {name: hashlib.sha256((apps / name).read_bytes()).hexdigest()
                               for name in copies}}


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps', type=Path, required=True)
    print(apply(parser.parse_args().apps))
