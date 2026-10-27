#!/usr/bin/env python3
"""Pinned-SDK integration for eth0-only hotspot NAT44 and bounded entries."""
from pathlib import Path
import hashlib,json

def digest(data):return hashlib.sha256(data).hexdigest()

def apply(nuttx):
    root=Path(nuttx)
    originals={
      'net/Makefile':'12ca48bd2e5641d9f816d924c9a118a774064c4b23ac626721b5e95078915c7f',
      'net/netforward/ipv4_forward.c':'ae388e529ea9d6db51a9cce772ac545cb471ae0273afb152e280ec61ca0b721a',
      'net/netforward/netforward.h':'d0b0191f1601692ddadbb2b221e6e5ff2a177173ce475d2e2ba82cfa569b2f4d',
      'net/netforward/netfwd_forward.c':'8bdddda690e48fb582754c2b9681f5d616c7b99b7c6ee45c49133324b72d2514',
      'net/nat/nat.c':'6e249441a4da00c220a34ddfd03b2d0e3e4604d5c6dc682a8fa1e757c5b1f896',
      'net/nat/ipv4_nat.c':'99f2e1716909074c72ba5c62fea8a7e060495d6b85ceddb566be6967d2de9359',
      'net/tcp/tcp_conn.c':'8d983be33e43ecaa08ffa6394943e6c04f3428f5f475f8efce755387f4511d71',
      'net/udp/udp_conn.c':'7c9ea350e0d4d28903860bf807c9eef1d701bb23c4815d9cd87153695795d832',
      'net/nat/ipv4_nat_entry.c':'e188dbcc8d101f03d43f2688149c652c5eb6c859e10ca91774a3b8558c2940de'}
    stamp=root/'net/.espclaw-share-source.json'
    expected={**originals,**json.loads(stamp.read_text())} if stamp.exists() else originals
    for name,sha in expected.items():
        if digest((root/name).read_bytes())!=sha:
            raise ValueError('Changed pinned sharing input: '+name)
    # Each transformation is repeatable and rejects changed anchors. The
    # reference hashes are checked before preparing an independent candidate.
    changes={}
    def edit(name,old,new):
        p=root/name;text=p.read_text()
        if new not in text:
            if text.count(old)!=1:raise ValueError('Unknown network sharing anchor: '+name)
            text=text.replace(old,new,1);p.write_text(text)
        changes[name]=digest(p.read_bytes())
    edit('net/Makefile','NET_CSRCS = net_initialize.c',
         'NET_CSRCS = net_initialize.c\nifeq ($(CONFIG_NET_NAT44),y)\nNET_CSRCS += espclaw_share.c\nendif')
    edit('net/netforward/ipv4_forward.c','#include <nuttx/config.h>',
         '#include <nuttx/config.h>\n#ifdef CONFIG_NET_NAT44\n#  include <nuttx/net/espclaw_share.h>\n#endif')
    edit('net/netforward/ipv4_forward.c',
         '  fwddev     = netdev_findby_ripv4addr(srcipaddr, destipaddr);',
         '#ifdef CONFIG_NET_NAT44\n  fwddev = espclaw_share_forward(dev, srcipaddr, destipaddr);\n#else\n  fwddev     = netdev_findby_ripv4addr(srcipaddr, destipaddr);\n#endif')
    # Callback allocation mutates both target device lists. Publishing it
    # before locking the target races the target poller's removal of the
    # last callback, leaving owned callbacks unreachable from d_conncb.
    # Busy targets receive a per-packet work item; the forwarding pool bounds
    # these items, and the worker owns the packet until callback publication.
    edit('net/netforward/netforward.h', '#include <stdint.h>',
         '#include <stdint.h>\n#include <nuttx/wqueue.h>')
    edit('net/netforward/netforward.h',
         '  FAR struct forward_s        *f_flink;',
         '  struct work_s               f_work;    /* Deferred target enqueue */\n'
         '  FAR struct forward_s        *f_flink;')
    path = root/'net/netforward/netfwd_forward.c'
    text = path.read_text()
    if 'netfwd_publish_locked' not in text:
        def replace_function(text, signature, replacement):
            start = text.index(signature)
            pos = text.index('{', start); depth = 1; end = pos + 1
            while depth:
                depth += (text[end] == '{') - (text[end] == '}')
                end += 1
            return text[:start] + replacement + text[end:]
        worker = r'''static int netfwd_publish_locked(FAR struct forward_s *fwd)
{
  FAR struct net_driver_s *dev = fwd->f_dev;

  if (!IFF_IS_UP(dev->d_flags))
    {
      return -ENETDOWN;
    }

  fwd->f_cb = netfwd_callback_alloc(dev);
  if (fwd->f_cb == NULL)
    {
      return -EBUSY;
    }

  fwd->f_cb->flags = IPFWD_POLL | NETDEV_DOWN;
  fwd->f_cb->priv = fwd;
  fwd->f_cb->event = netfwd_eventhandler;
  netdev_txnotify_dev(dev, IPFWD_POLL);
  return OK;
}

static void netfwd_forward_work(void *arg)
{
  FAR struct forward_s *fwd = arg;
  FAR struct net_driver_s *dev = fwd->f_dev;

  /* No input-device lock is held here. Callback lists and poll flags share
   * the target lock. Do not access fwd after publication: synchronous
   * notification may already have consumed and recycled it.
   */

  netdev_lock(dev);
  if (netfwd_publish_locked(fwd) < 0)
    {
      netfwd_dropstats(fwd);
      iob_free_chain(fwd->f_iob);
      netfwd_free(fwd);
    }

  netdev_unlock(dev);
}'''
        forward = r'''int netfwd_forward(FAR struct net_driver_s *dev, FAR struct forward_s *fwd)
{
  FAR struct net_driver_s *target = fwd->f_dev;
  FAR struct iob_s *iob;
  FAR uint8_t *buf;
  int len;
  int ret;

  DEBUGASSERT(fwd != NULL && fwd->f_iob != NULL && target != NULL);

  /* Never wait for a second device while holding the input device lock. */

  if (nxrmutex_trylock(&target->d_lock) != OK)
    {
      return work_queue(LPWORK, &fwd->f_work, netfwd_forward_work, fwd, 0);
    }

  iob = dev->d_iob;
  buf = dev->d_buf;
  len = dev->d_len;
  dev->d_iob = NULL;
  dev->d_buf = NULL;
  dev->d_len = 0;
  ret = netfwd_publish_locked(fwd);
  dev->d_iob = iob;
  dev->d_buf = buf;
  dev->d_len = len;
  netdev_unlock(target);
  return ret;
}'''
        text = replace_function(text, 'static void netfwd_forward_work(', worker)
        text = replace_function(text, 'int netfwd_forward(', forward)
        path.write_text(text)
    changes['net/netforward/netfwd_forward.c'] = digest(path.read_bytes())
    edit('net/nat/nat.c','      return -EEXIST;',
         '      nat_unlock();\n      return -EEXIST;')
    # Port selection re-enters nat_port_inuse through TCP/UDP selectors.
    # The pinned stack therefore requires owner-recursive NAT locking.
    edit('net/nat/nat.c',
         'static DEFINE_PER_CPU_BMP(mutex_t, g_nat_lock) = NXMUTEX_INITIALIZER;',
         'static DEFINE_PER_CPU_BMP(rmutex_t, g_nat_lock) = NXRMUTEX_INITIALIZER;')
    edit('net/nat/nat.c','  nxmutex_lock(&g_nat_lock);',
         '  nxrmutex_lock(&g_nat_lock);')
    edit('net/nat/nat.c','  nxmutex_unlock(&g_nat_lock);',
         '  nxrmutex_unlock(&g_nat_lock);')
    edit('net/nat/ipv4_nat.c','          return -ENOENT;',
         '          nat_unlock();\n          return -ENOENT;')
    # Every port selector must acquire NAT before the connection table.
    # Forwarding already holds NAT when selecting a port. The public
    # wrapper also handles all early returns in the original selector.
    for name,typ,signature,args in [
      ('tcp','int','uint8_t domain, FAR const union ip_addr_u *ipaddr, uint16_t portno, bool reuseaddr','domain, ipaddr, portno, reuseaddr'),
      ('udp','uint16_t','uint8_t domain, FAR union ip_binding_u *u','domain, u')]:
        selector='tcp_selectport' if name=='tcp' else 'udp_select_port'
        path=root/('net/'+name+'/'+name+'_conn.c')
        text=path.read_text();marker='/* ESPClaw NAT-before-port-table gate */'
        if marker not in text:
            begin=text.index(typ+' '+selector+'(')
            opening=text.index('{',begin);end=opening+1;depth=1
            while depth:
                depth+=(text[end]=='{')-(text[end]=='}');end+=1
            original=text[begin:end]
            internal='static '+original.replace(selector+'(',selector+'_internal(',1)
            wrapper='''\n\n%s
%s %s(%s)
{
  %s result;
#ifdef CONFIG_NET_NAT
  nat_lock();
#endif
  result = %s_internal(%s);
#ifdef CONFIG_NET_NAT
  nat_unlock();
#endif
  return result;
}'''%(marker,typ,selector,signature,typ,selector,args)
            text=text[:begin]+internal+wrapper+text[end:];path.write_text(text)
        changes[str(path.relative_to(root))]=digest(path.read_bytes())
    # Entries are CPU metadata, not DMA buffers. In this flat firmware the
    # user heap is PSRAM; keep mappings away from the small internal kernel
    # heap. The hard cap still bounds memory and lookup work for two clients.
    # Count and diagnostics use the same native NAT lock as both hash tables.
    if '#include <nuttx/net/espclaw_share.h>' not in (root/'net/nat/ipv4_nat_entry.c').read_text():
        edit('net/nat/ipv4_nat_entry.c','#include "nat/nat.h"',
             '#include "nat/nat.h"\n#include <nuttx/net/espclaw_share.h>\n\nstatic unsigned g_share_entries;')
    if 'entry = kumm_malloc(' not in (root/'net/nat/ipv4_nat_entry.c').read_text():
        edit('net/nat/ipv4_nat_entry.c',
             '  FAR ipv4_nat_entry_t *entry = kmm_malloc(sizeof(ipv4_nat_entry_t));',
             '  FAR ipv4_nat_entry_t *entry;\n  if (g_share_entries >= 64) return NULL;\n  entry = kmm_malloc(sizeof(ipv4_nat_entry_t));')
        edit('net/nat/ipv4_nat_entry.c','  entry->protocol      = protocol;',
             '  g_share_entries++;\n  entry->protocol      = protocol;')
        edit('net/nat/ipv4_nat_entry.c','  kmm_free(entry);',
             '  g_share_entries--;\n  kmm_free(entry);')
    # Upgrade the earlier 64-entry preparation as well as pristine inputs.
    edit('net/nat/ipv4_nat_entry.c','static unsigned g_share_entries;',
         '''#ifndef CONFIG_BUILD_FLAT
#  error ESPClaw sharing requires the flat-build PSRAM user heap
#endif
static unsigned g_share_entries;
static unsigned g_share_peak;
static uint32_t g_share_limit_hits;
static uint32_t g_share_alloc_failures;''')
    edit('net/nat/ipv4_nat_entry.c',
         '  if (g_share_entries >= 64) return NULL;\n  entry = kmm_malloc(sizeof(ipv4_nat_entry_t));',
         '''  if (g_share_entries >= ESPCLAW_SHARE_NAT_LIMIT)
    {
      g_share_limit_hits++;
      return NULL;
    }

  entry = kumm_malloc(sizeof(ipv4_nat_entry_t));''')
    edit('net/nat/ipv4_nat_entry.c',
         '      nwarn("WARNING: Failed to allocate IPv4 NAT entry\\n");',
         '      g_share_alloc_failures++;\n      nwarn("WARNING: Failed to allocate IPv4 NAT entry\\n");')
    edit('net/nat/ipv4_nat_entry.c','  g_share_entries++;\n  entry->protocol',
         '''  g_share_entries++;
  if (g_share_peak < g_share_entries)
    {
      g_share_peak = g_share_entries;
    }

  entry->protocol''')
    edit('net/nat/ipv4_nat_entry.c','  g_share_entries--;\n  kmm_free(entry);',
         '  g_share_entries--;\n  kumm_free(entry);')
    stats='''
void espclaw_share_nat_stats(FAR struct espclaw_share_nat_s *out)
{
  nat_lock();
  out->entries = g_share_entries;
  out->peak = g_share_peak;
  out->limit = ESPCLAW_SHARE_NAT_LIMIT;
  out->limit_hits = g_share_limit_hits;
  out->alloc_failures = g_share_alloc_failures;
  nat_unlock();
}

'''
    edit('net/nat/ipv4_nat_entry.c','#endif /* CONFIG_NET_NAT44 */',
         stats+'#endif /* CONFIG_NET_NAT44 */')
    # A minimal address-only flush avoids constructing a large net_driver_s
    # on the small worker stack and handles an already-replaced DHCP IP.
    extra='''
static void share_clear_cb(FAR ipv4_nat_entry_t *entry, FAR void *arg)
{
  if (entry->external_ip == *(FAR in_addr_t *)arg)
    ipv4_nat_entry_delete(entry);
}

void espclaw_share_clear_entries(in_addr_t address)
{
  nat_lock();
  ipv4_nat_entry_foreach(share_clear_cb, &address);
  nat_unlock();
}

'''
    edit('net/nat/ipv4_nat_entry.c','void ipv4_nat_entry_clear(FAR struct net_driver_s *dev)',
         extra+'void ipv4_nat_entry_clear(FAR struct net_driver_s *dev)')
    stamp.write_text(json.dumps(changes,indent=2)+'\n')
    return {'enabled':True,'wan':'eth0','lan':'ap0','nat_entry_limit':2048,
            'nat_entry_heap':'bounded PSRAM user heap (flat build)',
            'recursive_port_selection_lock':True,'outbound_failure_unlock':True,
            'port_table_lock_order':'NAT gate before TCP/UDP connection table',
            'source_sha256':changes}

if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--nuttx',type=Path,required=True)
    print(json.dumps(apply(p.parse_args().nuttx),indent=2))
