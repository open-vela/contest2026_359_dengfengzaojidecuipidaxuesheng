/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "c6_ota.h"
#include "c6_ota_release.h"

int c6_ota_command(int argc, char *argv[])
{
  struct c6_ota_result_s result = {0};
  struct stat st;
  uint8_t *image = NULL;
  FILE *file = NULL;
  int apply;
  int ret;

  if (argc != 5 || (strcmp(argv[2], "check") != 0 &&
                    strcmp(argv[2], "apply") != 0))
    {
      fprintf(stderr, "usage: c6probe ota check|apply /sdcard/... <sha256>\n");
      return EXIT_FAILURE;
    }

  apply = strcmp(argv[2], "apply") == 0;
  if (strncmp(argv[3], "/sdcard/", 8) != 0)
    {
      ret = -EINVAL;
      goto done;
    }

  file = fopen(argv[3], "rb");
  if (file == NULL)
    {
      ret = -errno;
      goto done;
    }

  if (fstat(fileno(file), &st) < 0)
    {
      ret = -errno;
      goto done;
    }

  if (!S_ISREG(st.st_mode) || st.st_size < 64 ||
      st.st_size > C6_OTA_SLOT_SIZE)
    {
      ret = -EFBIG;
      goto done;
    }

  /* Own a complete bounded copy before any Flash operation. Removal or
   * mutation of the SD source cannot change the validated update bytes.
   */

  image = malloc((size_t)st.st_size);
  if (image == NULL)
    {
      ret = -ENOMEM;
      goto done;
    }

  if (fread(image, 1, (size_t)st.st_size, file) != (size_t)st.st_size ||
      fgetc(file) != EOF || ferror(file))
    {
      ret = -EIO;
      goto done;
    }

  ret = fclose(file) == 0 ? 0 : -EIO;
  file = NULL;
  if (ret < 0) goto done;
  ret = c6_ota_validate(image, (size_t)st.st_size, argv[4]);
  if (ret < 0) goto done;
  if ((size_t)st.st_size != C6_OTA_RELEASE_BYTES ||
      strcmp(argv[4], C6_OTA_RELEASE_SHA256) != 0)
    {
      ret = -EPERM;
      goto done;
    }
  printf("c6-ota: validated bytes=%lu chip=esp32c6 integrity=sha256\n",
         (unsigned long)st.st_size);
  if (apply)
    {
      ret = esp_hosted_rpc_c6_ota(image, (size_t)st.st_size, &result);
    }

done:
  if (file != NULL) fclose(file);
  free(image);
  printf("c6-ota: result=%d acknowledged=%lu begin=%d commit_attempted=%d "
         "committed=%d\n", ret, (unsigned long)result.acknowledged,
         result.begin_attempted, result.commit_attempted, result.committed);
  if (result.begin_attempted)
    printf("c6-ota: restart P4 before another maintenance attempt; "
           "do not replay an uncertain commit\n");
  return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
