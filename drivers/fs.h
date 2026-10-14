// #ifndef FS_H
// #define FS_H
// 
// #include <../libc/stdint>
// #include <../libc/string.h>
// #include <../libc/mem.h>
// #include <screen.h>
// 
// #define NULL 0
// 
// #define TYPE_FILE 0
// #define TYPE_DIR  1
// #define FILE_TABLE_SIZE 1024
// #define BLOCK_SIZE 512
// #define MAX_FILENAME_LENGTH 32
// 
// 
// #define readSector(d, l, b) device_read(floppy?'F':'H', d,l,b)
// #define writeSector(d, l, b) device_write(floppy?'F':'H', d,l,b)
// 
// // ��������� ������ �����/����������
// struct FileEntry {
// 	char name[MAX_FILENAME_LENGTH]; // ��� ����� ��� ����������
// 	uint32_t size;                  // ������ ����� � ������ (��� ���������� ��� 0)
// 	uint32_t start_block;           // ����� ���������� ����� ������
// 	uint8_t is_used;                // ����, �����������, ������������ �� ������
// 	uint8_t type;                   // ���: ���� (TYPE_FILE) ��� ���������� (TYPE_DIR)
// 	uint32_t parent_dir;            // ����� ������ ������������ ����������
// };
// 
// extern uint8_t type;
// extern char floppy;
// extern struct FileEntry file_table[FILE_TABLE_SIZE]; // ������� ������
// 
// // ��������� �������
// uint32_t allocate_block();
// void free_blocks(uint32_t start_block);
// int create_file_or_dir(const char* name, uint8_t type, uint32_t parent_dir_index);
// void delete_file_or_dir(uint32_t index);
// void list_directory(uint32_t dir_index);
// void ls_n(uint32_t dir_index, uint16_t param, char[]);
// uint32_t ls(uint32_t dir_index, uint16_t param);
// 
// struct FileEntry* find_file_or_dir(const char* name, uint32_t dir_index);
// int obj2idx(const char* name, uint32_t dir_index);
// 
// void write_file(uint32_t file_index, const uint8_t* data, uint32_t size);
// void read_file(uint32_t file_index, uint8_t* buffer, uint32_t size);
// 
// #define ERROR_NOT_A_DIR -1
// 
// #define ERROR_NOT_A_NUMBER -2
// #define ERROR_NOT_A_STRING -4
// 
// #define ERROR_OUT_OF_RANGE -3
// 
// // | param |  type  | 
// // +-------+--------+----------------------- - - -
// // |   0   | string | parent directory name
// // |   1   | string | directory in 
// 
// 
// #endif // FS_H
