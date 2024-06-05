#include "cmd.h"
#include "utils.h"
#include "mem.h"

char** tokens;

void cmd(char* cmd, uint64_t len){
    
    char* token = strtok(cmd, " ");
    token = remove_nl(token);
    //the first token is the command
    if(strEql(token, "clear")){
        clear();
    }
    else if(strEql(token, "echo")){
        while ((token = strtok(NULL, " ")) != ""){
            printf("{s} ", token);
        }
    }
   
}

void clear(){
    for(int i = 0; i < 80; i++){
        printf("{n}");
    }
    
}