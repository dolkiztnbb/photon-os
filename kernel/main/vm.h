char* vm_ptr = 0;
long long regs[16]={0};


void set_vm_ptr(int addr){
    vm_ptr = (char*)addr;
}

void vm_write(char data[], int length, int pos){
    for(int x=0;x<length;x++){
        vm_ptr[x+pos]=data[x];
    }
}

void vm_read(char *buffer, int length, int pos){
    for(int x=0;x<length;x++){
        buffer[x]=vm_ptr[x+pos];
    }
}

long long vm_rv(int pos){
    long long temp;
    temp|=vm_ptr[pos+3];
    temp|=vm_ptr[pos+2]<<8;
    temp|=vm_ptr[pos+1]<<16;
    temp|=vm_ptr[pos]<<24;
    return temp;
}

void set_reg(char reg, long long value){
    regs[reg]=value;
}

long long read_reg(char reg){
    return regs[reg];
}


