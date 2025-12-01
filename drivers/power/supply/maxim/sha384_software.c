/*******************************************************************************
* Copyright (C) 2017 Maxim Integrated Products, Inc., All Rights Reserved.
* Copyright (C) 2021 XiaoMi, Inc.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL MAXIM INTEGRATED BE LIABLE FOR ANY CLAIM, DAMAGES
* OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*
* Except as contained in this notice, the name of Maxim Integrated
* Products, Inc. shall not be used except as stated in the Maxim Integrated
* Products, Inc. Branding Policy.
*
* The mere transfer of this software does not imply any licenses
* of trade secrets, proprietary technology, copyrights, patents,
* trademarks, maskwork rights, or any other form of intellectual
* property whatsoever. Maxim Integrated Products, Inc. retains all
* ownership rights.
*******************************************************************************
*/
//  SHA3_HMAC - HMAC using SHA3-256
#include "ucl_sha3.h"
#include <linux/string.h>
#include <linux/slab.h>
#define SHA3_256_HMAC
#include "sha384_software.h"

/* Static buffers to avoid stack overflow and kmalloc overhead */
static unsigned char __aligned(64) hmac_thash[256];
static unsigned char __aligned(64) hmac_tmac[256];
static unsigned char __aligned(64) hmac_cat_thash[1024];
static unsigned char __aligned(64) hmac_cat_final[1024];
static DEFINE_SPINLOCK(hmac_lock);

//---------------------------------------------------------------------------
/// Compute HMAC using SHA3-256.
///
/// @param[in] key
/// buffer for key
/// @param[in] ken_len
/// length of key
/// @param[in] message
/// buffer for message
/// @param[in] msg_len
/// length of message
/// @param[out] mac
/// 32 byte output mac
///
/// Restrictions:
///  Key length limited to 32 bytes.
///  Message Length is limited to 512 bytes.
///
/// @return
/// TRUE - command successful @n
/// FALSE - command failed
///
int sha3_256_hmac(unsigned char *key, int key_len, unsigned char *message, int msg_len, unsigned char *mac)
{
	const int blocksize = 136;
	const int hashsize = 32;
	unsigned long flags;
	int i;
	unsigned char opad[136] __aligned(8);
	unsigned char ipad[136] __aligned(8);

	/* Early validation - no lock needed */
	if (unlikely(key_len > blocksize || msg_len > 512))
		return 0;

	/* Use static buffers with spinlock for thread safety */
	spin_lock_irqsave(&hmac_lock, flags);

	memset(opad, 0x5C, blocksize);
	memset(ipad, 0x36, blocksize);

	/* XOR ipad/opad with key */
	for (i = 0; i < key_len; i++) {
		ipad[i] ^= key[i];
		opad[i] ^= key[i];
	}

	/* thash = hash(ipad || message) */
	memcpy(hmac_cat_thash, ipad, blocksize);
	memcpy(hmac_cat_thash + blocksize, message, msg_len);
	ucl_sha3_256(hmac_thash, hmac_cat_thash, blocksize + msg_len);

	/* mac = hash(opad || thash) */
	memcpy(hmac_cat_final, opad, blocksize);
	memcpy(hmac_cat_final + blocksize, hmac_thash, hashsize);
	ucl_sha3_256(hmac_tmac, hmac_cat_final, blocksize + hashsize);

	memcpy(mac, hmac_tmac, hashsize);

	spin_unlock_irqrestore(&hmac_lock, flags);

	return 1;
}
