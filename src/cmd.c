#include "cmd.h"
#include "utils.h"
#include "mem.h"
#include "usoft/info.h"

void cmd(char* cmd, uint64_t len){
    //reset strtok
    char* tmp = strtok(NULL, " ");
    char* token = strtok(cmd, " ");     
    token = remove_nl(token);
    //the first token is the command
    if(strEql(token, "clear")){
        clear();
    }
    else if(strEql(token, "echo")){
        token = strtok(NULL, " ");
        while (!strEql((token), "\0")){
            printf("{s}", token);
            token = strtok(NULL, " ");
            if(!strEql(token, "\0")){
                printf(" ");
            }
        }
    }
    else if(strEql(token, "neofetch"))
    {
        print_info();
    }
    else if(strEql(token, "timertest")){
        timer_test();
    }
    strtok(NULL, NULL);
}

void clear(){
    ft_ctx->clear(ft_ctx, false);
}

void timer_test(){
    uint64_t start = hpet_get_ticks();
    uint64_t t = hpet_get_ticks();
    while(t - start < 1000){
        printf("{dn}",t - start);
        t = hpet_get_ticks();
    };
    printf("Timer test passed\n");
}