#ifndef BIT_H
#define BIT_H

unsigned int get_top_bits(unsigned int value, int num_bits);
unsigned int get_mid_bits(unsigned int value, int mid_bits, int low_bits);
unsigned int get_low_bits(unsigned int value, int num_bits);
void set_bit_at_index(char *bitmap, int index);
void clear_bit_at_index(char *bitmap, int index);
int get_bit_at_index(char *bitmap, int index);

#endif
