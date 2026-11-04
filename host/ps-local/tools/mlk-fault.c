#include "mlk_fault_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc!=2) { fprintf(stderr,"usage: mlk-fault '{JSON API v1/v2 request}'\n"); return 2; }
    char reply[4096];
    const char *socket=getenv("MLK_FAULT_SOCKET");
    const int v2=strstr(argv[1],"\"api\":2")!=NULL;
    const int rc=v2 ? mlk_fault_publish_json_v2(socket,argv[1],reply,sizeof(reply),2000)
                    : mlk_fault_publish_json(socket,argv[1],reply,sizeof(reply),2000);
    if (rc) { perror("mlk-fault"); return 1; }
    fputs(reply,stdout);
    return strstr(reply,"\"ok\":true") ? 0 : 1;
}
