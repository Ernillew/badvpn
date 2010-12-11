/**
 * @file dump_frame.h
 * @author Ambroz Bizjak <ambrop7@gmail.com>
 * 
 * @section LICENSE
 * 
 * This file is part of BadVPN.
 * 
 * BadVPN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 * 
 * BadVPN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * 
 * @section DESCRIPTION
 * 
 * Function for dumping an Ethernet frame to a file in pcap format, used
 * for debugging (e.g. for analyzing with Wireshark).
 */

#ifndef BADVPN_MISC_DUMP_FRAME_H
#define BADVPN_MISC_DUMP_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

struct pcap_hdr {
    uint32_t magic_number;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;
} __attribute__((packed));

struct pcaprec_hdr {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
} __attribute__((packed));

static int write_to_file (uint8_t *data, size_t data_len, FILE *f)
{
    while (data_len > 0) {
        size_t bytes = fwrite(data, 1, data_len, f);
        if (bytes == 0) {
            return 0;
        }
        data += bytes;
        data_len -= bytes;
    }
    
    return 1;
}

static int dump_frame (uint8_t *data, size_t data_len, const char *file)
{
    FILE *f = fopen(file, "w");
    if (!f) {
        goto fail0;
    }
    
    struct pcap_hdr gh;
    gh.magic_number = 0xa1b2c3d4;
    gh.version_major = 2;
    gh.version_minor = 4;
    gh.thiszone = 0;
    gh.sigfigs = 0;
    gh.snaplen = 65535;
    gh.network = 1;
    
    if (!write_to_file((uint8_t *)&gh, sizeof(gh), f)) {
        goto fail1;
    }
    
    struct pcaprec_hdr ph;
    ph.ts_sec = 0;
    ph.ts_usec = 0;
    ph.incl_len = data_len;
    ph.orig_len = data_len;
    
    if (!write_to_file((uint8_t *)&ph, sizeof(ph), f)) {
        goto fail1;
    }
    
    if (!write_to_file(data, data_len, f)) {
        goto fail1;
    }
    
    if (fclose(f) != 0) {
        return 0;
    }
    
    return 1;
    
fail1:
    fclose(f);
fail0:
    return 0;
}

#endif
