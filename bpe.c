#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define SUCCESS 0
#define FILE_OPEN_ERROR -1
#define MEMORY_ALLOCATION_ERROR -2
#define FILE_WRITE_ERROR -3

#define FILE_PATH "temp/example"
#define VOCAB_PATH "temp/vocab"
#define DECODE_PATH "temp/decode"
#define MAX_PATH_LENGTH 4096
#define MAX_TABLE_SIZE 50000
#define MERGE_ITERATIONS 20
#define VOCAB_SIZE 256 + MERGE_ITERATIONS
#define MAX_QUEUE_SIZE VOCAB_SIZE

// This queue is used for decoding
typedef struct {
    uint16_t buffer[MAX_QUEUE_SIZE];
    uint16_t head;
    uint16_t tail;
} queue_t;

queue_t decode_queue = {.head = 0, .tail = 0};

typedef struct {
    uint16_t byte_1;
    uint16_t byte_2;
    uint16_t freq;
} byte_pair_t;

typedef struct {
    uint16_t byte_1;
    uint16_t byte_2;
} vocab_item_t;

// Training variables
byte_pair_t merge_table[MAX_TABLE_SIZE] = {0};
uint16_t merge_table_size = 0;
uint16_t *bytes_stream[2] = {NULL, NULL}; // We will use it alternatively for the original bytes stream and the new bytes stream
uint64_t bytes_stream_size = 0;
uint8_t current_bytes_stream = 0;
vocab_item_t vocab_table[VOCAB_SIZE] = {0};
uint16_t vocab_table_size = 256;  // 0-255 are the ASCII characters

// Decoding variables
uint16_t *decode_stream[2] = {NULL, NULL}; // First buffer is encoded, second buffer is decoded
uint64_t decode_stream_size = 0;
uint8_t current_decode_stream = 0;

uint16_t dequeue_byte(){
    if(decode_queue.head == decode_queue.tail){
        printf("ERROR: Decode queue is empty\n");
        exit(1);
    }
    uint16_t byte = decode_queue.buffer[decode_queue.head];
    decode_queue.head = (decode_queue.head + 1) % MAX_QUEUE_SIZE;
    return byte;
}

void enqueue_byte(uint16_t byte){
    if((decode_queue.tail + 1) % MAX_QUEUE_SIZE == decode_queue.head){
        printf("ERROR: Decode queue is full\n");
        exit(1);
    }
    decode_queue.buffer[decode_queue.tail] = byte;
    decode_queue.tail = (decode_queue.tail + 1) % MAX_QUEUE_SIZE;

    // Uncomment for debugging
    // printf("Enqueued byte: %u\n", byte);
}

uint64_t load_dataset() {
    // Open the file
    FILE *file = fopen(FILE_PATH ".txt", "rb");
    if (file == NULL) {
        perror("Could not open file"); 
        return FILE_OPEN_ERROR;
    }
    
    // Get the file size
    fseek(file, 0, SEEK_END);
    uint64_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    // Temporary 8-bit buffer
    uint8_t *tmp_byte_buffer = malloc(file_size);
    if (!tmp_byte_buffer) {
        perror("Could not allocate temporary byte buffer");
        fclose(file);
        return MEMORY_ALLOCATION_ERROR;
    }

    // Read the file into the bytes_stream
    fread(tmp_byte_buffer, 1, file_size, file);
    fclose(file);
    
    // Allocate 16-bit stream
    bytes_stream[0] = malloc(file_size * sizeof(uint16_t));
    bytes_stream[1] = malloc(file_size * sizeof(uint16_t)); // Will be used after the first iteration
    if (!bytes_stream[0]) {
        perror("Could not allocate bytes_stream_0");
        free(tmp_byte_buffer);
        return MEMORY_ALLOCATION_ERROR;
    }
    if (!bytes_stream[1]) {
        perror("Could not allocate bytes_stream_1");
        free(tmp_byte_buffer);
        return MEMORY_ALLOCATION_ERROR;
    }

    // Expand 8-bit → 16-bit
    for (uint64_t i = 0; i < file_size; i++) {
        bytes_stream[0][i] = (uint16_t)tmp_byte_buffer[i];
    }

    free(tmp_byte_buffer);
    bytes_stream_size = file_size;
    return SUCCESS;
}

int32_t write_bytes(uint16_t iteration, uint8_t is_binary) {
    // Create the path
    char path[MAX_PATH_LENGTH];
    if(is_binary){
        snprintf(path, MAX_PATH_LENGTH, "%s.bin%u", FILE_PATH, iteration);
    }
    else{
        snprintf(path, MAX_PATH_LENGTH, "%s.txt%u", FILE_PATH, iteration);
    }
    
    // Open the file
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        perror("Could not open file"); 
        return FILE_OPEN_ERROR;
    }
    // Write the bytes to the file
    if(is_binary){
        size_t writ_bytes = fwrite(bytes_stream[current_bytes_stream], sizeof(uint16_t), bytes_stream_size, file);
        if(writ_bytes != bytes_stream_size){
            printf("ERROR: Failed to write bytes to file: %ld\n", writ_bytes);
            fclose(file);
            return FILE_WRITE_ERROR;
        }
    }
    else{
        for (uint64_t i = 0; i < bytes_stream_size; i++) {
            fprintf(file, "%u ", bytes_stream[current_bytes_stream][i]);
        }
        fprintf(file, "\n");
    }
    fclose(file);
    return SUCCESS;
}

void print_bytes() {
    for (uint64_t i = 0; i < bytes_stream_size; i++) {
        printf("%d ", bytes_stream[current_bytes_stream][i]);
    }
    printf("\nSize: %lu\n", bytes_stream_size);
}

int32_t create_merge_table() {
    for (uint64_t i = 0; i < bytes_stream_size - 1; i++) {
        byte_pair_t pair = {bytes_stream[current_bytes_stream][i], bytes_stream[current_bytes_stream][i + 1], 1};
        uint64_t j = 0;
        for (j = 0; j < merge_table_size; j++) {
            if (merge_table[j].byte_1 == pair.byte_1 && merge_table[j].byte_2 == pair.byte_2) {
                merge_table[j].freq++;
                break;
            }
        }
        if (j == merge_table_size) {
            merge_table[merge_table_size++] = pair;
        }
    }
    return SUCCESS;
}

int32_t write_merge_table(uint16_t iteration) {
    // Create the path
    char path[MAX_PATH_LENGTH];
    snprintf(path, MAX_PATH_LENGTH, "%s.merges%u", FILE_PATH, iteration);
    
    // Open the file
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        perror("Could not open file"); 
        return FILE_OPEN_ERROR;
    }

    for (uint64_t i = 0; i < MAX_TABLE_SIZE; i++) {
        fprintf(file, "%u %u %u\n", merge_table[i].byte_1, merge_table[i].byte_2, merge_table[i].freq);
    }
    fclose(file);
    return SUCCESS;
}

int32_t sort_merge_table(){
    // Sort the merge table by frequency
    for(uint16_t i = 0; i < merge_table_size - 1; i++){
        for(uint16_t j = i + 1; j < merge_table_size; j++){
            if(merge_table[i].freq < merge_table[j].freq){
                byte_pair_t tmp = merge_table[i];
                merge_table[i] = merge_table[j];
                merge_table[j] = tmp;
            }
        }
    }
    return SUCCESS;
}

byte_pair_t get_max_merge_frequency(){
    byte_pair_t max_merge = {0, 0, 0};
    for (uint16_t i = 0; i < merge_table_size; i++){
        if (merge_table[i].freq > max_merge.freq){
            max_merge = merge_table[i];
        }
    }
    return max_merge;
}

int32_t write_vocab_table(uint16_t iteration){
    // Create the path
    char path[MAX_PATH_LENGTH];
    snprintf(path, MAX_PATH_LENGTH, "%s.vocab%u", FILE_PATH, iteration);

    // Open the file
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        perror("Could not open file"); 
        return FILE_OPEN_ERROR;
    }
    for(uint16_t i=0;i<VOCAB_SIZE-MERGE_ITERATIONS;i++){
        fprintf(file, "%u %u\n", i, i);
    }
    for(uint16_t i = VOCAB_SIZE-MERGE_ITERATIONS; i < vocab_table_size; i++){
        fprintf(file, "%u %u %u\n", i, vocab_table[i].byte_1, vocab_table[i].byte_2);
    }
    fclose(file);
    return SUCCESS;
}

void update_vocab_table(byte_pair_t max_merge){
    vocab_item_t item = {max_merge.byte_1, max_merge.byte_2};
    vocab_table[vocab_table_size++] = item;
}

void apply_best_merge(byte_pair_t best_merge){
    
    uint64_t new_bytes_stream_idx = 0;
    for(uint64_t i=0; i<bytes_stream_size-1; i++){
        if(bytes_stream[current_bytes_stream][i] == best_merge.byte_1 && bytes_stream[current_bytes_stream][i+1] == best_merge.byte_2){
            bytes_stream[!current_bytes_stream][new_bytes_stream_idx] = vocab_table_size - 1;
            new_bytes_stream_idx++;
            i++;
            continue;
        }
        bytes_stream[!current_bytes_stream][new_bytes_stream_idx++] = bytes_stream[current_bytes_stream][i];
    }
    bytes_stream[!current_bytes_stream][new_bytes_stream_idx] = bytes_stream[current_bytes_stream][bytes_stream_size-1];
    
    // Swap the bytes streams
    current_bytes_stream = !current_bytes_stream;
    bytes_stream_size = new_bytes_stream_idx + 1;
}

int32_t train_bpe(){

    // Load the dataset in bytes
    int32_t result = load_dataset();
    if (result != SUCCESS) {
        printf("Error loading dataset: %d\n", result);
        return result;
    }
    printf("Loaded %lu bytes from %s\n", bytes_stream_size, FILE_PATH);
    
    for(uint16_t i=0; i<MERGE_ITERATIONS; i++){
        printf("Merge iteration %u\n", i+1);
    
        // Write the bytes to a file
        // write_bytes(i+1, 0);
    
        // Get byte pairs from the bytes
        create_merge_table();
    
        // Sort the merge table
        sort_merge_table();
    
        // Write the sorted merge table
        // write_merge_table(i+1);
    
        // Get the max merge frequency
        byte_pair_t best_merge = get_max_merge_frequency();
    
        // Print the max merge frequency
        printf("Max merge frequency: %u %u %u\n", best_merge.byte_1, best_merge.byte_2, best_merge.freq);
    
        // Update the vocabulary table
        update_vocab_table(best_merge);
        
        // Write the vocabulary table
        // write_vocab_table(i+1);
    
        // Apply the best merge to the bytes stream
        apply_best_merge(best_merge);
    
        // Reset the merge table
        merge_table_size = 0;
    
        printf("Bytes stream size: %lu\n", bytes_stream_size);
    }
    
    // Write the final bytes to a file
    write_bytes(MERGE_ITERATIONS+1, 1);
    write_bytes(MERGE_ITERATIONS+1, 0);

    // Write the final vocabulary 
    write_vocab_table(MERGE_ITERATIONS+1);

    // Cleanup
    if (bytes_stream[0] != NULL) {
        free(bytes_stream[0]);
        bytes_stream[0] = NULL;
    }
    if (bytes_stream[1] != NULL) {
        free(bytes_stream[1]);
        bytes_stream[1] = NULL;
    }

    return SUCCESS;
}

int32_t load_vocab_table(){
    // Open the file
    FILE *file = fopen(VOCAB_PATH ".txt", "rb");
    if (file == NULL) {
        perror("Could not open file"); 
        return FILE_OPEN_ERROR;
    }
    
    // Read the file into the vocab_table
    uint16_t token_id, byte_1, byte_2;
    for(uint16_t i=0; i< 256;i++){
        fscanf(file, "%hu %hu", &token_id, &byte_1);
        vocab_table[token_id].byte_1 = vocab_table[token_id].byte_2 = byte_1;
    }
    for(uint16_t i = 256; i < VOCAB_SIZE; i++){
        fscanf(file, "%hu %hu %hu\n", &token_id, &byte_1, &byte_2);
        vocab_table[token_id].byte_1 = byte_1;
        vocab_table[token_id].byte_2 = byte_2;
    }
    fclose(file);
    vocab_table_size = VOCAB_SIZE;
    return SUCCESS;
}

int32_t load_decoding_stream(){
    // Open the file
    FILE *file = fopen(DECODE_PATH ".txt", "rb");
    if (file == NULL) {
        perror("Could not open file"); 
        return FILE_OPEN_ERROR;
    }

    // Get the file size
    fseek(file, 0, SEEK_END);
    uint64_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    // NOTE: We will use half of the file size for the decode stream
    file_size /= 2;

    // Allocate decode stream
    decode_stream[0] = malloc(file_size * sizeof(uint16_t));
    decode_stream[1] = malloc(file_size * sizeof(uint16_t)); // Will be used after the first iteration
    if (!decode_stream[0]) {
        perror("Could not allocate decode_stream_0");
        return MEMORY_ALLOCATION_ERROR;
    }

    // Read the file into the decode stream
    fread(decode_stream[0], sizeof(uint16_t), file_size, file);
    fclose(file);

    decode_stream_size = file_size;
    return SUCCESS;
}

void decode_byte_recursively(uint16_t byte){
    
    // Base case: ASCII character
    if(byte < 256){
        enqueue_byte(byte);
        return;
    }

    // Find the byte pair in the vocabulary table
    if(byte >= vocab_table_size){
        printf("ERROR: Byte not found in vocabulary table: %u\n", byte);
        exit(1);
    }
    vocab_item_t item = vocab_table[byte];
    decode_byte_recursively(item.byte_1);
    decode_byte_recursively(item.byte_2);
    return;
}

void decode_decoding_stream(){
    // Decode the decoding stream
    uint16_t new_decode_stream_idx = 0;
    for(uint64_t i=0; i<decode_stream_size; i++){
        // Decode the byte
        decode_byte_recursively(decode_stream[current_decode_stream][i]);
        // Copy the decoded bytes to the decoding stream
        while(decode_queue.head != decode_queue.tail){
            decode_stream[!current_decode_stream][new_decode_stream_idx] = dequeue_byte();
            new_decode_stream_idx++;
        }
    }
}

int32_t write_decoded_stream(){
    // Create the path
    char path[MAX_PATH_LENGTH];
    snprintf(path, MAX_PATH_LENGTH, "%s.decoded", DECODE_PATH);

    // Open the file
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        perror("Could not open file"); 
        return FILE_OPEN_ERROR;
    }

    // Write the decoded bytes to the file
    for(uint64_t i=0; i<decode_stream_size; i++){
        fprintf(file, "%c", (char)decode_stream[!current_decode_stream][i]);
    }
    fclose(file);
    return SUCCESS;
}

void decode(){
    // Load the vocabulary table
    load_vocab_table();
    printf("Loaded %u tokens from %s\n", vocab_table_size, VOCAB_PATH);

    // write_vocab_table(0);
    // printf("Written %u tokens to %s\n", vocab_table_size, VOCAB_PATH);
    
    // Load the bytes stream
    load_decoding_stream();
    printf("Loaded %lu bytes from %s\n", decode_stream_size, DECODE_PATH);

    for(uint64_t i=0; i<decode_stream_size; i++){
        printf("%u ", decode_stream[current_decode_stream][i]);
    }
    printf("\n");

    // Decode bytes stream
    decode_decoding_stream();

    // Write the decoded bytes to a file
    write_decoded_stream();
}

int main() {
    clock_t start_time, end_time;
    double cpu_time_used;

    start_time = clock();

    // TRAIN
    train_bpe();

    // DECODE
    // decode();

    end_time = clock();
    // Calculate the elapsed time in seconds
    cpu_time_used = ((double) (end_time - start_time)) / CLOCKS_PER_SEC;

    printf("Start time (ticks): %ld\n", (long)start_time);
    printf("End time (ticks): %ld\n", (long)end_time);
    printf("Execution time was %f seconds\n", cpu_time_used);
    return SUCCESS;
}