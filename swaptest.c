#include "param.h"
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"
#include "syscall.h"
#include "traps.h"

#include "memlayout.h"
#define ITER 30000

char* arr[ITER];

int main () {
    int a,b;
    swapstat(&a,&b);
    printf(1,"swapstat %d %d\n",a,b);
    exit();
}