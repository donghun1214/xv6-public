#include "param.h"
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"
#include "syscall.h"
#include "traps.h"

#include "memlayout.h"
#define ITER 600

char* arr[ITER];

int main () {
    int *k = (int*)malloc(4);
    for (int i =0 ; i<ITER;i++){
        char* p = sbrk(4096*2);
        if(p==(char*)-1) break;
        *p = 'c';
        arr[i]=p;
        if(i%100 == 0)
            printf(1, "Now Allocating %d\n", i);
    }
    printf(1,"allocated done\n");
    int pid = fork();
    if(pid == 0){
        for(int i=0;i<ITER;i+=100){
            printf(1,"Child print %d : %x ->%c\n",i,(int)arr[i],*arr[i]);
        }
	exit();
    }else{
	wait();
	for(int i=0;i<ITER;i+=100){
            printf(1,"Parent print %d : %x ->%c\n",i,(int)arr[i],*arr[i]);
        }

    }
    printf(1,"read after swapping %x %d\n",(int)k,*k);
    int a,b;
    swapstat(&a,&b);
    printf(1,"swapstat %d %d\n",a,b);
    exit();
}