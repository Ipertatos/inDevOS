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
    strtok(NULL, NULL);
}

void clear(){
    for(int i = 0; i < 80; i++){
        printf("{n}");
    }
    
}