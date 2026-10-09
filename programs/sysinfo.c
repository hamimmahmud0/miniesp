// sysinfo - system status as one line of JSON (used by the web dashboard: /cgi-bin/sysinfo).
#include "mini.h"

int main(int argc, char **argv)
{
    char host[32], ip[20], ssid[34];
    sys_netinfo(0, host, sizeof host); sys_netinfo(1, ip, sizeof ip); sys_netinfo(2, ssid, sizeof ssid);
    m_printf("{\"host\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"uptime\":%d,\"mhz\":%d,\"tasks\":%d,"
             "\"dram_free\":%d,\"dram_total\":%d,\"iram_free\":%d,\"iram_total\":%d,\"fs_used\":%d,\"fs_total\":%d}\n",
             host, ip, ssid, sys_sysinfo(SI_RSSI), sys_sysinfo(SI_UPTIME_S), sys_sysinfo(SI_CPU_MHZ), sys_sysinfo(SI_NTASKS),
             sys_sysinfo(SI_DRAM_FREE), sys_sysinfo(SI_DRAM_TOTAL), sys_sysinfo(SI_IRAM8_FREE), sys_sysinfo(SI_IRAM8_TOTAL),
             sys_sysinfo(SI_FS_USED), sys_sysinfo(SI_FS_TOTAL));
    return 0;
}
