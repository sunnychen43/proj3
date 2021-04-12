unsigned int get_top_bits(unsigned int value, int num_bits) {
    return (value >> (32 - num_bits));  // 32 assuming we are using 32-bit address
}

unsigned int get_mid_bits(unsigned int value, int mid_bits, int low_bits) {
    unsigned int mask = (1 << mid_bits) - 1;  
    return (value >> low_bits) & mask;
}

unsigned int get_low_bits(unsigned int value, int num_bits) {
    return value & ((1<<num_bits)-1);
}

void set_bit_at_index(char *bitmap, int index) {
    *bitmap |= (1 << index);
}

void clear_bit_at_index(char *bitmap, int index) {
    *bitmap &= ~(1 << index);
}

int get_bit_at_index(char *bitmap, int index) {
    return (*bitmap >> index) & 1;
}