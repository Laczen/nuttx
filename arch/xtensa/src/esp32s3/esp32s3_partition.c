/****************************************************************************
 * arch/xtensa/src/esp32s3/esp32s3_partition.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <string.h>
#include <debug.h>
#include <stdio.h>
#include <errno.h>

#include <nuttx/kmalloc.h>

#include "esp32s3_spiflash_mtd.h"
#include "esp32s3_partition.h"
#include "esp32s3_spiflash.h"
#include "arch/esp32s3/partition.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Partition table max size */

#define PARTITION_MAX_SIZE    (0xc00)

/* Partition max number */

#define PARTITION_MAX_NUM     (PARTITION_MAX_SIZE / \
                               sizeof(struct partition_info_priv_s))

/* Partition table header magic value */

#define PARTITION_MAGIC       (0x50aa)

/* Partition table member label length */

#define PARTITION_LABEL_LEN   (16)

/* OTA data offset in OTA partition */

#define OTA_DATA_OFFSET       (4096)

/* OTA data number */

#define OTA_DATA_NUM          (2)

/* Partition offset in SPI Flash */

#define PARTITION_TABLE_OFFSET CONFIG_ESP32S3_PARTITION_TABLE_OFFSET

/* Partition MTD device mount point */

#define PARTITION_MOUNT_POINT CONFIG_ESP32S3_PARTITION_MOUNTPT

/* Partition encrypted flag */

#define PARTITION_FLAG_ENCRYPTED          (1 << 0)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* OTA image state */

enum ota_img_state_e
{
  /* Monitor the first boot. In bootloader of esp-idf this state is changed
   * to ESP_OTA_IMG_PENDING_VERIFY if this bootloader enable app rollback.
   *
   * So this driver doesn't use this state currently.
   */

  OTA_IMG_NEW             = 0x0,

  /* First boot for this app was. If while the second boot this state is then
   * it will be changed to ABORTED if this bootloader enable app rollback.
   *
   * So this driver doesn't use this state currently.
   */

  OTA_IMG_PENDING_VERIFY  = 0x1,

  /* App was confirmed as workable. App can boot and work without limits. */

  OTA_IMG_VALID           = 0x2,

  /* App was confirmed as non-workable. This app will not selected to boot. */

  OTA_IMG_INVALID         = 0x3,

  /* App could not confirm the workable or non-workable. In bootloader
   * IMG_PENDING_VERIFY state will be changed to IMG_ABORTED. This app will
   * not selected to boot at all if this bootloader enable app rollback.
   *
   * So this driver doesn't use this state currently.
   */

  OTA_IMG_ABORTED         = 0x4,

  /* Undefined. App can boot and work without limits in esp-idf.
   *
   * This state is not used.
   */

  OTA_IMG_UNDEFINED       = 0xffffffff,
};

/* Partition information data */

struct partition_info_priv_s
{
  uint16_t magic;                       /* Partition magic */
  uint8_t  type;                        /* Partition type */
  uint8_t  subtype;                     /* Partition sub-type */

  uint32_t offset;                      /* Offset in SPI Flash */
  uint32_t size;                        /* Size by byte */

  uint8_t  label[PARTITION_LABEL_LEN];  /* Partition label */

  uint32_t flags;                       /* Partition flags */
};

/* Partition device data */

struct mtd_dev_priv_s
{
  struct mtd_dev_s mtd;                 /* MTD data */

  uint8_t  type;                        /* Partition type */
  uint8_t  subtype;                     /* Partition sub-type */
  uint32_t flags;                       /* Partition flags */
  uint32_t offset;                      /* Partition offset in SPI Flash */
  uint32_t size;                        /* Partition size in SPI Flash */
  struct mtd_dev_s  *ll_mtd;            /* Low-level MTD data */
  struct mtd_dev_s  *part_mtd;          /* Partition MTD data */
  struct mtd_geometry_s   geo;          /* Partition geometry information */
};

/* OTA data entry */

struct ota_data_entry_s
{
  uint32_t ota_seq;                     /* Boot sequence */
  uint8_t  seq_label[20];               /* Boot sequence label */
  uint32_t ota_state;                   /* Boot entry state */
  uint32_t crc;                         /* Boot ota_seq CRC32 */
};

/****************************************************************************
 * External Function Prototypes
 ****************************************************************************/

extern uint32_t crc32_le(uint32_t crc, uint8_t const *buf, uint32_t len);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ota_is_valid
 *
 * Description:
 *   Check if OTA data is valid
 *
 * Input Parameters:
 *   ota_data - OTA data
 *
 * Returned Value:
 *   true if checking success or false if fail
 *
 ****************************************************************************/

static bool ota_is_valid(struct ota_data_entry_s *ota_data)
{
  if ((ota_data->ota_seq >= OTA_IMG_BOOT_SEQ_MAX) ||
      (ota_data->ota_state != OTA_IMG_VALID) ||
      (ota_data->crc != crc32_le(UINT32_MAX, (uint8_t *)ota_data, 4)))
    {
      return false;
    }

  return true;
}

/****************************************************************************
 * Name: ota_get_bootseq
 *
 * Description:
 *   Get boot sequence
 *
 * Input Parameters:
 *   dev - Partition private MTD data
 *   num - boot sequence buffer
 *
 * Returned Value:
 *   0 if success or a negative value if fail.
 *
 ****************************************************************************/

static int ota_get_bootseq(struct mtd_dev_priv_s *dev, int *num)
{
  int i;
  int ret;
  struct ota_data_entry_s ota_data;
  int size = sizeof(struct ota_data_entry_s);

  /* Each OTA data locates in independent sector */

  for (i = 0; i < OTA_DATA_NUM; i++)
    {
      ret = MTD_READ(dev->part_mtd, i * OTA_DATA_OFFSET,
                     size, (uint8_t *)&ota_data);
      if (ret != size)
        {
          ferr("ERROR: Failed to read OTA%d data error=%d\n", i, ret);
          return -EIO;
        }

      if (ota_is_valid(&ota_data))
        {
          *num = i + OTA_IMG_BOOT_OTA_0;
          break;
        }
    }

  if (i >= 2)
    {
      *num = OTA_IMG_BOOT_FACTORY;
    }

  return 0;
}

/****************************************************************************
 * Name: ota_set_bootseq
 *
 * Description:
 *   Set boot sequence
 *
 * Input Parameters:
 *   dev - Partition private MTD data
 *   num - boot sequence buffer
 *
 * Returned Value:
 *   0 if success or a negative value if fail.
 *
 ****************************************************************************/

static int ota_set_bootseq(struct mtd_dev_priv_s *dev, int num)
{
  int ret;
  int id;
  int old_id;
  struct ota_data_entry_s ota_data;
  int size = sizeof(struct ota_data_entry_s);

  finfo("INFO: num=%d\n", num);

  switch (num)
    {
      case OTA_IMG_BOOT_FACTORY:

        /* Erase all OTA data to force use factory app */

        ret = MTD_ERASE(dev->part_mtd, 0, OTA_DATA_NUM);
        if (ret != OTA_DATA_NUM)
          {
            ferr("ERROR: Failed to erase OTA data error=%d\n", ret);
            return -EIO;
          }

        break;
      case OTA_IMG_BOOT_OTA_0:
      case OTA_IMG_BOOT_OTA_1:
        {
          id = num - 1;
          old_id = num == OTA_IMG_BOOT_OTA_0 ? OTA_IMG_BOOT_OTA_1 - 1:
                                               OTA_IMG_BOOT_OTA_0 - 1;

          ret = MTD_ERASE(dev->part_mtd, id, 1);
          if (ret != 1)
            {
              ferr("ERROR: Failed to erase OTA%d data error=%d\n", id, ret);
              return -EIO;
            }

          ota_data.ota_state = OTA_IMG_VALID;
          ota_data.ota_seq = num;
          ota_data.crc = crc32_le(UINT32_MAX, (uint8_t *)&ota_data, 4);
          ret = MTD_WRITE(dev->part_mtd, id * OTA_DATA_OFFSET,
                          size, (uint8_t *)&ota_data);
          if (ret != size)
            {
              ferr("ERROR: Failed to write OTA%d data error=%d\n",
                   id, ret);
              return -1;
            }

          /* Erase old OTA data to force new OTA bin */

          ret = MTD_ERASE(dev->part_mtd, old_id, 1);
          if (ret != 1)
            {
              ferr("ERROR: Failed to erase OTA%d data error=%d\n",
                   old_id, ret);
              return -EIO;
            }
        }

        break;
      default:
        ferr("ERROR: num=%d is error\n", num);
        return -EINVAL;
    }

    return 0;
}

/****************************************************************************
 * Name: ota_invalidate_bootseq
 *
 * Description:
 *   Invalidate boot sequence by deleting the corresponding otadata
 *
 * Input Parameters:
 *   dev - Partition private MTD data
 *   num - boot sequence buffer
 *
 * Returned Value:
 *   0 if success or a negative value if fail.
 *
 ****************************************************************************/

static int ota_invalidate_bootseq(struct mtd_dev_priv_s *dev, int num)
{
  int ret;
  int id;

  finfo("INFO: num=%d\n", num);

  switch (num)
    {
      case OTA_IMG_BOOT_OTA_0:
      case OTA_IMG_BOOT_OTA_1:
        {
          id = num - 1;

          ret = MTD_ERASE(dev->part_mtd, id, 1);
          if (ret != 1)
            {
              ferr("ERROR: Failed to erase OTA%d data error=%d\n",
                   id, ret);
              return -EIO;
            }
        }

        break;
      default:
        ferr("ERROR: num=%d is error\n", num);
        return -EINVAL;
    }

    return 0;
}

/****************************************************************************
 * Name: is_currently_mapped_as_text
 *
 * Description:
 *   Check if the MTD partition is mapped as text
 *
 * Input Parameters:
 *   dev    - Partition private MTD data
 *   mapped - true if mapped, false if not
 *
 * Returned Value:
 *   0 if success or a negative value if fail.
 *
 ****************************************************************************/

static int is_currently_mapped_as_text(struct mtd_dev_priv_s *dev,
                                       bool *mapped)
{
  uint32_t currently_mapped_address;

  if (mapped == NULL)
    {
      ferr("ERROR: Invalid argument.\n");
      return -EINVAL;
    }

  currently_mapped_address = esp32s3_get_flash_address_mapped_as_text();

  *mapped = ((dev->offset <= currently_mapped_address) &&
             (currently_mapped_address < dev->offset + dev->size));

  return OK;
}

/****************************************************************************
 * Name: esp32s3_part_erase
 *
 * Description:
 *   Erase SPI Flash designated sectors.
 *
 * Input Parameters:
 *   dev        - ESP32-S3 MTD device data
 *   startblock - start block number, it is not equal to SPI Flash's block
 *   nblocks    - blocks number
 *
 * Returned Value:
 *   0 if success or a negative value if fail.
 *
 ****************************************************************************/

static int esp32s3_part_erase(struct mtd_dev_s *dev, off_t startblock,
                              size_t nblocks)
{
  struct mtd_dev_priv_s *mtd_priv = (struct mtd_dev_priv_s *)dev;

  return MTD_ERASE(mtd_priv->ll_mtd, startblock, nblocks);
}

/****************************************************************************
 * Name: esp32s3_part_read
 *
 * Description:
 *   Read data from SPI Flash at designated address.
 *
 * Input Parameters:
 *   dev    - ESP32-S3 MTD device data
 *   offset - target address offset
 *   nbytes - data number
 *   buffer - data buffer pointer
 *
 * Returned Value:
 *   Read data bytes if success or a negative value if fail.
 *
 ****************************************************************************/

static ssize_t esp32s3_part_read(struct mtd_dev_s *dev, off_t offset,
                                 size_t nbytes, uint8_t *buffer)
{
  struct mtd_dev_priv_s *mtd_priv = (struct mtd_dev_priv_s *)dev;

  return MTD_READ(mtd_priv->ll_mtd, offset, nbytes, buffer);
}

/****************************************************************************
 * Name: esp32s3_part_bread
 *
 * Description:
 *   Read data from designated blocks.
 *
 * Input Parameters:
 *   dev        - ESP32-S3 MTD device data
 *   startblock - start block number, it is not equal to SPI Flash's block
 *   nblocks    - blocks number
 *   buffer     - data buffer pointer
 *
 * Returned Value:
 *   Read block number if success or a negative value if fail.
 *
 ****************************************************************************/

static ssize_t esp32s3_part_bread(struct mtd_dev_s *dev,  off_t startblock,
                                  size_t nblocks, uint8_t *buffer)
{
  struct mtd_dev_priv_s *mtd_priv = (struct mtd_dev_priv_s *)dev;

  return MTD_BREAD(mtd_priv->ll_mtd, startblock, nblocks, buffer);
}

/****************************************************************************
 * Name: esp32s3_part_write
 *
 * Description:
 *   write data to SPI Flash at designated address.
 *
 * Input Parameters:
 *   dev    - ESP32-S3 MTD device data
 *   offset - target address offset
 *   nbytes - data number
 *   buffer - data buffer pointer
 *
 * Returned Value:
 *   Written bytes if success or a negative value if fail.
 *
 ****************************************************************************/

static ssize_t esp32s3_part_write(struct mtd_dev_s *dev, off_t offset,
                                  size_t nbytes, const uint8_t *buffer)
{
  struct mtd_dev_priv_s *mtd_priv = (struct mtd_dev_priv_s *)dev;

  return MTD_WRITE(mtd_priv->ll_mtd, offset, nbytes, buffer);
}

/****************************************************************************
 * Name: esp32s3_part_bwrite
 *
 * Description:
 *   Write data to designated blocks.
 *
 * Input Parameters:
 *   dev        - ESP32-S3 MTD device data
 *   startblock - start MTD block number,
 *                it is not equal to SPI Flash's block
 *   nblocks    - blocks number
 *   buffer     - data buffer pointer
 *
 * Returned Value:
 *   Written block number if success or a negative value if fail.
 *
 ****************************************************************************/

static ssize_t esp32s3_part_bwrite(struct mtd_dev_s *dev, off_t startblock,
                                   size_t nblocks, const uint8_t *buffer)
{
  struct mtd_dev_priv_s *mtd_priv = (struct mtd_dev_priv_s *)dev;

  return MTD_BWRITE(mtd_priv->ll_mtd, startblock, nblocks, buffer);
}

/****************************************************************************
 * Name: esp32s3_part_ioctl
 *
 * Description:
 *   Set/Get/Invaliate option to/from ESP32-S3 SPI Flash MTD device data.
 *
 * Input Parameters:
 *   dev - ESP32-S3 MTD device data
 *   cmd - operation command
 *   arg - operation argument
 *
 * Returned Value:
 *   0 if success or a negative value if fail.
 *
 ****************************************************************************/

static int esp32s3_part_ioctl(struct mtd_dev_s *dev, int cmd,
                              unsigned long arg)
{
  int ret;
  struct mtd_dev_priv_s *mtd_priv = (struct mtd_dev_priv_s *)dev;

  finfo("INFO: cmd=%d(0x%x) arg=%lx\n", cmd, cmd, arg);

  switch (_IOC_NR(cmd))
    {
      case OTA_IMG_GET_BOOT:
        {
          int *num = (int *)arg;

          ret = ota_get_bootseq(mtd_priv, num);
          if (ret < 0)
            {
              ferr("ERROR: Failed to get boot img\n");
            }
        }

        break;
      case OTA_IMG_SET_BOOT:
        {
          ret = ota_set_bootseq(mtd_priv, arg);
          if (ret < 0)
            {
              ferr("ERROR: Failed to set boot img\n");
            }
        }

        break;
      case OTA_IMG_INVALIDATE_BOOT:
        {
          ret = ota_invalidate_bootseq(mtd_priv, arg);
          if (ret < 0)
            {
              ferr("ERROR: Failed to invalidate boot img\n");
            }
        }

        break;
      case OTA_IMG_IS_MAPPED_AS_TEXT:
        {
          bool *mapped = (bool *)arg;

          ret = is_currently_mapped_as_text(mtd_priv, mapped);
          if (ret < 0)
            {
              ferr("ERROR: Failed to check partition is mapped as text\n");
            }
        }

        break;
      default:
        {
          ret = MTD_IOCTL(mtd_priv->ll_mtd, cmd, arg);
        }

        break;
    }

  return ret;
}

/****************************************************************************
 * Name: partition_get_offset
 *
 * Description:
 *   Get offset in SPI flash of the partition label
 *
 * Input Parameters:
 *   label - Partition label
 *   size  - Data number
 *
 * Returned Value:
 *   Get partition offset(>= 0) if success or a negative value if fail.
 *
 ****************************************************************************/

static int partition_get_offset(const char *label, size_t size)
{
  int i;
  int ret;
  uint8_t *pbuf;
  int partion_offset;
  const struct partition_info_priv_s *info;
  DEBUGASSERT(label != NULL);
  struct mtd_dev_s *mtd = esp32s3_spiflash_encrypt_mtd();
  if (!mtd)
    {
      ferr("ERROR: Failed to get SPI flash MTD\n");
      return -ENOSYS;
    }

  pbuf = kmm_malloc(PARTITION_MAX_SIZE);
  if (!pbuf)
    {
      ferr("ERROR: Failed to allocate %d byte\n", PARTITION_MAX_SIZE);
      return -ENOMEM;
    }

  ret = MTD_READ(mtd, PARTITION_TABLE_OFFSET,
                 PARTITION_MAX_SIZE, pbuf);
  if (ret != PARTITION_MAX_SIZE)
    {
      ferr("ERROR: Failed to get read data from MTD\n");
      kmm_free(pbuf);
      return -EIO;
    }

  info = (struct partition_info_priv_s *)pbuf;
  for (i = 0; i < PARTITION_MAX_NUM; i++)
    {
      if (memcmp(info[i].label, label, size) == 0)
        {
          partion_offset = info[i].offset;
          break;
        }
    }

  kmm_free(pbuf);
  if (i == PARTITION_MAX_NUM)
    {
      ferr("ERROR: No %s  partition is created\n", label);
      return -EPERM;
    }

  finfo("Get Partition offset: 0x%x\n", partion_offset);
  return partion_offset;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_partition_init
 *
 *   Initialize ESP32-S3 partition. Read partition information, and use
 *   these data for creating MTD.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   0 if success or a negative value if fail.
 *
 ****************************************************************************/

int esp32s3_partition_init(void)
{
  int i;
  struct partition_info_priv_s *info;
  uint8_t *pbuf;
  bool encrypt;
  uint32_t flags;
  struct mtd_dev_s *mtd;
  struct mtd_dev_s *mtd_ll;
  struct mtd_dev_s *mtd_encrypt;
  struct mtd_geometry_s geo;
  struct mtd_dev_priv_s *mtd_priv;
  int ret = 0;
  const int num = PARTITION_MAX_SIZE / sizeof(struct partition_info_priv_s);
  const char path_base[] = PARTITION_MOUNT_POINT;
  char label[PARTITION_LABEL_LEN + 1];
  char path[PARTITION_LABEL_LEN + sizeof(PARTITION_MOUNT_POINT)];

  pbuf = kmm_malloc(PARTITION_MAX_SIZE);
  if (!pbuf)
    {
      ferr("ERROR: Failed to allocate %d byte\n", PARTITION_MAX_SIZE);
      ret = -ENOMEM;
      goto errout_with_malloc;
    }

  mtd = esp32s3_spiflash_mtd();
  if (!mtd)
    {
      ferr("ERROR: Failed to get SPI flash MTD\n");
      ret = -ENOSYS;
      goto errout_with_mtd;
    }

  mtd_encrypt = esp32s3_spiflash_encrypt_mtd();
  if (!mtd_encrypt)
    {
      ferr("ERROR: Failed to get SPI flash encrypted MTD\n");
      ret = -ENOSYS;
      goto errout_with_mtd;
    }

  /* Even without SPI Flash encryption, we can also use encrypted
   * MTD to read no-encrypted data.
   */

  ret = MTD_READ(mtd_encrypt, PARTITION_TABLE_OFFSET,
                 PARTITION_MAX_SIZE, pbuf);
  if (ret != PARTITION_MAX_SIZE)
    {
      ferr("ERROR: Failed to get read data from MTD\n");
      ret = -EIO;
      goto errout_with_mtd;
    }

  info = (struct partition_info_priv_s *)pbuf;
  encrypt = esp32s3_flash_encryption_enabled();
  for (i = 0; i < num; i++)
    {
      char *name;

      if (info->magic != PARTITION_MAGIC)
        {
          break;
        }

      strlcpy(label, (char *)info->label, sizeof(label));
      snprintf(path, sizeof(path), "%s%s", path_base, label);
      mtd_ll = mtd;

      /* If SPI Flash encryption is enable, "APP", "OTA data" and "NVS keys"
       * are force to set as encryption partition.
       */

      flags = info->flags;
      if (encrypt)
        {
          if ((info->type == PARTITION_TYPE_DATA &&
              info->subtype == PARTITION_SUBTYPE_DATA_OTA) ||
              (info->type == PARTITION_TYPE_DATA &&
              info->subtype == PARTITION_SUBTYPE_DATA_NVS_KEYS))
            {
              flags |= PARTITION_FLAG_ENCRYPTED;
            }
        }

      finfo("INFO: [label]:   %s\n", label);
      finfo("INFO: [type]:    %d\n", info->type);
      finfo("INFO: [subtype]: %d\n", info->subtype);
      finfo("INFO: [offset]:  0x%08" PRIx32 "\n", info->offset);
      finfo("INFO: [size]:    0x%08" PRIx32 "\n", info->size);
      finfo("INFO: [flags]:   0x%08" PRIx32 "\n", info->flags);
      finfo("INFO: [mount]:   %s\n", path);
      if (encrypt && (flags & PARTITION_FLAG_ENCRYPTED))
        {
          mtd_ll = mtd_encrypt;
          finfo("INFO: [encrypted]\n\n");
        }
      else
        {
          mtd_ll = mtd;
          finfo("INFO: [no-encrypted]\n\n");
        }

      ret = MTD_IOCTL(mtd_ll, MTDIOC_GEOMETRY, (unsigned long)&geo);
      if (ret < 0)
        {
          ferr("ERROR: Failed to get info from MTD\n");
          goto errout_with_mtd;
        }

      mtd_priv = kmm_malloc(sizeof(struct mtd_dev_priv_s));
      if (!mtd_priv)
        {
          ferr("ERROR: Failed to allocate %d byte\n",
               sizeof(struct mtd_dev_priv_s));
          ret = -ENOMEM;
          goto errout_with_mtd;
        }

      mtd_priv->offset  = info->offset;
      mtd_priv->size    = info->size;
      mtd_priv->type    = info->type;
      mtd_priv->subtype = info->subtype;
      mtd_priv->flags   = flags;
      mtd_priv->ll_mtd  = mtd_ll;
      memcpy(&mtd_priv->geo, &geo, sizeof(geo));

      mtd_priv->mtd.bread  = esp32s3_part_bread;
      mtd_priv->mtd.bwrite = esp32s3_part_bwrite;
      mtd_priv->mtd.erase  = esp32s3_part_erase;
      mtd_priv->mtd.ioctl  = esp32s3_part_ioctl;
      mtd_priv->mtd.read   = esp32s3_part_read;
      mtd_priv->mtd.write  = esp32s3_part_write;
      mtd_priv->mtd.name   = name = strdup(label);
      if (!name)
        {
          ferr("ERROR: Failed to allocate MTD name\n");
          kmm_free(mtd_priv);
          ret = -ENOSPC;
          goto errout_with_mtd;
        }

      mtd_priv->part_mtd = mtd_partition(&mtd_priv->mtd,
                                         info->offset / geo.blocksize,
                                         info->size / geo.blocksize);
      if (!mtd_priv->part_mtd)
        {
          ferr("ERROR: Failed to create MTD partition\n");
          lib_free(name);
          kmm_free(mtd_priv);
          ret = -ENOSPC;
          goto errout_with_mtd;
        }

      ret = register_mtddriver(path, mtd_priv->part_mtd, 0777, NULL);
      if (ret < 0)
        {
          ferr("ERROR: Failed to register MTD @ %s\n", path);
          lib_free(name);
          kmm_free(mtd_priv);
          goto errout_with_mtd;
        }

      info++;
    }

  ret = 0;

errout_with_mtd:
  kmm_free(pbuf);

errout_with_malloc:
  return ret;
}

/****************************************************************************
 * Name: esp32s3_partition_read
 *
 * Description:
 *   Read data from SPI Flash at designated address.
 *
 * Input Parameters:
 *   label  - Partition label
 *   offset - Offset in SPI Flash
 *   buf    - Data buffer pointer
 *   size   - Data number
 *
 * Returned Value:
 *   0 if success or a negative value if fail.
 *
 ****************************************************************************/

int esp32s3_partition_read(const char *label, size_t offset, void *buf,
                           size_t size)
{
  int ret;
  int partion_offset;
  DEBUGASSERT(label != NULL && buf != NULL);
  struct mtd_dev_s *mtd = esp32s3_spiflash_mtd();
  if (!mtd)
    {
      ferr("ERROR: Failed to get SPI flash MTD\n");
      return -ENOSYS;
    }

  partion_offset = partition_get_offset(label, sizeof(label));
  if (partion_offset < 0)
    {
      ferr("ERROR: Failed to get partition: %s offset\n", label);
      return partion_offset;
    }

  ret = MTD_READ(mtd, partion_offset + offset,
                 size, (uint8_t *)buf);
  if (ret != size)
    {
      ferr("ERROR: Failed to get read data from MTD\n");
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: esp32s3_partition_write
 *
 * Description:
 *   Write data to SPI Flash at designated address.
 *
 * Input Parameters:
 *   label  - Partition label
 *   offset - Offset in SPI Flash
 *   buf    - Data buffer pointer
 *   size   - Data number
 *
 * Returned Value:
 *   0 if success or a negative value if fail.
 *
 ****************************************************************************/

int esp32s3_partition_write(const char *label, size_t offset, void *buf,
                            size_t size)
{
  int ret;
  int partion_offset;
  DEBUGASSERT(label != NULL && buf != NULL);
  struct mtd_dev_s *mtd = esp32s3_spiflash_mtd();
  if (!mtd)
    {
      ferr("ERROR: Failed to get SPI flash MTD\n");
      return -ENOSYS;
    }

  partion_offset = partition_get_offset(label, sizeof(label));
  if (partion_offset < 0)
    {
      ferr("ERROR: Failed to get partition: %s offset\n", label);
      return partion_offset;
    }

  ret = MTD_WRITE(mtd, partion_offset + offset,
                  size, (uint8_t *)buf);
  if (ret != size)
    {
      ferr("ERROR: Failed to get read data from MTD\n");
      return -EIO;
    }

  return OK;
}
