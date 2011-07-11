/**
 * @file NCDIfConfig.c
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
 */

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <pwd.h>
  
#include <misc/debug.h>
#include <base/BLog.h>

#include <ncd/NCDIfConfig.h>

#include <generated/blog_channel_NCDIfConfig.h>

#define IP_CMD "ip"
#define RESOLVCONF_FILE "/etc/resolv.conf"
#define RESOLVCONF_TEMP_FILE "/etc/resolv.conf-ncd-temp"
#define TUN_DEVNODE "/dev/net/tun"

static int run_command (const char *cmd)
{
    BLog(BLOG_INFO, "run: %s", cmd);
    
    return system(cmd);
}

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

int NCDIfConfig_query (const char *ifname)
{
    struct ifreq ifr;
    
    int flags = 0;
    
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (!s) {
        BLog(BLOG_ERROR, "socket failed");
        goto fail0;
    }
    
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(s, SIOCGIFFLAGS, &ifr)) {
        BLog(BLOG_ERROR, "SIOCGIFFLAGS failed");
        goto fail1;
    }
    
    flags |= NCDIFCONFIG_FLAG_EXISTS;
    
    if ((ifr.ifr_flags&IFF_UP)) {
        flags |= NCDIFCONFIG_FLAG_UP;
        
        if ((ifr.ifr_flags&IFF_RUNNING)) {
            flags |= NCDIFCONFIG_FLAG_RUNNING;
        }
    }
    
fail1:
    close(s);
fail0:
    return flags;
}

int NCDIfConfig_set_up (const char *ifname)
{
    if (strlen(ifname) >= IFNAMSIZ) {
        BLog(BLOG_ERROR, "ifname too long");
        return 0;
    }
    
    char cmd[50 + IFNAMSIZ];
    sprintf(cmd, IP_CMD" link set %s up", ifname);
    
    return !run_command(cmd);
}

int NCDIfConfig_set_down (const char *ifname)
{
    if (strlen(ifname) >= IFNAMSIZ) {
        BLog(BLOG_ERROR, "ifname too long");
        return 0;
    }
    
    char cmd[50 + IFNAMSIZ];
    sprintf(cmd, IP_CMD" link set %s down", ifname);
    
    return !run_command(cmd);
}

int NCDIfConfig_add_ipv4_addr (const char *ifname, struct ipv4_ifaddr ifaddr)
{
    ASSERT(ifaddr.prefix >= 0)
    ASSERT(ifaddr.prefix <= 32)
    
    if (strlen(ifname) >= IFNAMSIZ) {
        BLog(BLOG_ERROR, "ifname too long");
        return 0;
    }
    
    uint8_t *addr = (uint8_t *)&ifaddr.addr;
    
    char cmd[50 + IFNAMSIZ];
    sprintf(cmd, IP_CMD" addr add %"PRIu8".%"PRIu8".%"PRIu8".%"PRIu8"/%d dev %s", addr[0], addr[1], addr[2], addr[3], ifaddr.prefix, ifname);
    
    return !run_command(cmd);
}

int NCDIfConfig_remove_ipv4_addr (const char *ifname, struct ipv4_ifaddr ifaddr)
{
    ASSERT(ifaddr.prefix >= 0)
    ASSERT(ifaddr.prefix <= 32)
    
    if (strlen(ifname) >= IFNAMSIZ) {
        BLog(BLOG_ERROR, "ifname too long");
        return 0;
    }
    
    uint8_t *addr = (uint8_t *)&ifaddr.addr;
    
    char cmd[50 + IFNAMSIZ];
    sprintf(cmd, IP_CMD" addr del %"PRIu8".%"PRIu8".%"PRIu8".%"PRIu8"/%d dev %s", addr[0], addr[1], addr[2], addr[3], ifaddr.prefix, ifname);
    
    return !run_command(cmd);
}

static int route_cmd (const char *cmdtype, struct ipv4_ifaddr dest, const uint32_t *gateway, int metric, const char *device)
{
    ASSERT(dest.prefix >= 0)
    ASSERT(dest.prefix <= 32)
    
    uint8_t *d_addr = (uint8_t *)&dest.addr;
    
    char gwstr[60];
    if (gateway) {
        const uint8_t *g_addr = (uint8_t *)gateway;
        sprintf(gwstr, " via %"PRIu8".%"PRIu8".%"PRIu8".%"PRIu8, g_addr[0], g_addr[1], g_addr[2], g_addr[3]);
    } else {
        gwstr[0] = '\0';
    }
    
    char cmd[100];
    sprintf(cmd, IP_CMD" route %s %"PRIu8".%"PRIu8".%"PRIu8".%"PRIu8"/%d%s metric %d dev %s",
            cmdtype, d_addr[0], d_addr[1], d_addr[2], d_addr[3], dest.prefix, gwstr, metric, device);
    
    return !run_command(cmd);
}

int NCDIfConfig_add_ipv4_route (struct ipv4_ifaddr dest, const uint32_t *gateway, int metric, const char *device)
{
    return route_cmd("add", dest, gateway, metric, device);
}

int NCDIfConfig_remove_ipv4_route (struct ipv4_ifaddr dest, const uint32_t *gateway, int metric, const char *device)
{
    return route_cmd("del", dest, gateway, metric, device);
}

int NCDIfConfig_set_dns_servers (uint32_t *servers, size_t num_servers)
{
    FILE *temp_file = fopen(RESOLVCONF_TEMP_FILE, "w");
    if (!temp_file) {
        BLog(BLOG_ERROR, "failed to open resolvconf temp file");
        goto fail0;
    }
    
    char line[60];
    
    sprintf(line, "# generated by badvpn-ncd\n");
    if (!write_to_file((uint8_t *)line, strlen(line), temp_file)) {
        BLog(BLOG_ERROR, "failed to write to resolvconf temp file");
        goto fail1;
    }
    
    for (size_t i = 0; i < num_servers; i++) {
        uint8_t *addr = (uint8_t *)&servers[i];
        sprintf(line, "nameserver %"PRIu8".%"PRIu8".%"PRIu8".%"PRIu8"\n",
                addr[0], addr[1], addr[2], addr[3]);
        if (!write_to_file((uint8_t *)line, strlen(line), temp_file)) {
            BLog(BLOG_ERROR, "failed to write to resolvconf temp file");
            goto fail1;
        }
    }
    
    if (fclose(temp_file) != 0) {
        BLog(BLOG_ERROR, "failed to close resolvconf temp file");
        return 0;
    }
    
    if (rename(RESOLVCONF_TEMP_FILE, RESOLVCONF_FILE) < 0) {
        BLog(BLOG_ERROR, "failed to rename resolvconf temp file to resolvconf file");
        return 0;
    }
    
    return 1;
    
fail1:
    fclose(temp_file);
fail0:
    return 0;
}

static int open_tuntap (const char *ifname, int flags)
{
    if (strlen(ifname) >= IFNAMSIZ) {
        BLog(BLOG_ERROR, "ifname too long");
        return -1;
    }
    
    int fd = open(TUN_DEVNODE, O_RDWR);
    if (fd < 0) {
         BLog(BLOG_ERROR, "open tun failed");
         return -1;
    }
    
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = flags;
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    
    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
        BLog(BLOG_ERROR, "TUNSETIFF failed");
        close(fd);
        return -1;
    }
    
    return fd;
}

int NCDIfConfig_make_tuntap (const char *ifname, const char *owner, int tun)
{
    // load tun module if needed
    if (access(TUN_DEVNODE, F_OK) < 0) {
        if (run_command("/sbin/modprobe tun") != 0) {
            BLog(BLOG_ERROR, "modprobe tun failed");
        }
    }
    
    int fd;
    if ((fd = open_tuntap(ifname, (tun ? IFF_TUN : IFF_TAP))) < 0) {
        goto fail0;
    }
    
    if (owner) {
        long bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
        if (bufsize < 0) {
            bufsize = 16384;
        }
        
        char *buf = malloc(bufsize);
        if (!buf) {
            BLog(BLOG_ERROR, "malloc failed");
            goto fail1;
        }
        
        struct passwd pwd;
        struct passwd *res;
        getpwnam_r(owner, &pwd, buf, bufsize, &res);
        if (!res) {
            BLog(BLOG_ERROR, "getpwnam_r failed");
            free(buf);
            goto fail1;
        }
        
        int uid = pwd.pw_uid;
        
        free(buf);
        
        if (ioctl(fd, TUNSETOWNER, uid) < 0) {
            BLog(BLOG_ERROR, "TUNSETOWNER failed");
            goto fail1;
        }
    }
    
    if (ioctl(fd, TUNSETPERSIST, (void *)1) < 0) {
        BLog(BLOG_ERROR, "TUNSETPERSIST failed");
        goto fail1;
    }
    
    close(fd);
    
    return 1;
    
fail1:
    close(fd);
fail0:
    return 0;
}

int NCDIfConfig_remove_tuntap (const char *ifname, int tun)
{
    int fd;
    if ((fd = open_tuntap(ifname, (tun ? IFF_TUN : IFF_TAP))) < 0) {
        goto fail0;
    }
    
    if (ioctl(fd, TUNSETPERSIST, (void *)0) < 0) {
        BLog(BLOG_ERROR, "TUNSETPERSIST failed");
        goto fail1;
    }
    
    close(fd);
    
    return 1;
    
fail1:
    close(fd);
fail0:
    return 0;
}
