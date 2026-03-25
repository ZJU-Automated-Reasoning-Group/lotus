#include <stdio.h>
#include <string.h>
#include <unistd.h>

void nisse_pass_print_data(long long *count_array, int *index_array, int size) {
  if (access("main.prof", F_OK) != 0) {
    printf("Writing '%s'...\n", "main.prof");
  }
  FILE *file = fopen("main.prof", "a");
  if (!file) {
    perror("Could not open file");
    return;
  }
  
  for (int i = 0; i < size; i++) {
    fprintf(file, "%d %Ld\n", index_array[i], count_array[i]);
  } 
  fclose(file);
}