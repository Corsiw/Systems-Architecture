# Отчет по программе копирования файлов Терехов Дмитрий Семинар 7

## Исходный код программы

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#define BUFFER_SIZE 32

int main(int argc, char *argv[]) {
    int src_fd, dest_fd;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read, bytes_written;
    struct stat src_stat;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <source> <destination>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Get source file statistics (including permissions)
    if (stat(argv[1], &src_stat) == -1) {
        perror("Error getting source file statistics");
        exit(EXIT_FAILURE);
    }

    // Open source file for reading
    src_fd = open(argv[1], O_RDONLY);
    if (src_fd == -1) {
        perror("Error opening source file");
        exit(EXIT_FAILURE);
    }

    // Open destination file for writing
    dest_fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, src_stat.st_mode);
    if (dest_fd == -1) {
        perror("Error opening destination file");
        close(src_fd);
        exit(EXIT_FAILURE);
    }

    // Copy file content
    while ((bytes_read = read(src_fd, buffer, BUFFER_SIZE)) > 0) {
        bytes_written = write(dest_fd, buffer, bytes_read);
        if (bytes_written != bytes_read) {
            perror("Error writing to destination file");
            close(src_fd);
            close(dest_fd);
            exit(EXIT_FAILURE);
        }
    }

    if (bytes_read == -1) {
        perror("Error reading from source file");
        close(src_fd);
        close(dest_fd);
        exit(EXIT_FAILURE);
    }

    printf("File copied successfully!\n");
    printf("Permissions: %o\n", src_stat.st_mode & 0777);

    // Close files
    close(src_fd);
    close(dest_fd);
    return 0;
}
```

## Краткое описание программы

### Функциональность
Программа представляет собой утилиту для копирования файлов, использующую исключительно системные вызовы операционной системы. Она копирует содержимое исходного файла в файл назначения, сохраняя при этом права доступа исходного файла.

### Ключевые особенности:
- **Системные вызовы**: Использует `open()`, `read()`, `write()`, `close()`, `stat()`
- **Буфер ограниченного размера**: 32 байта (BUFFER_SIZE)
- **Циклическое чтение/запись**: Обработка файлов любого размера через цикл
- **Сохранение прав доступа**: Копирует режимы доступа исходного файла
- **Обработка ошибок**: Комплексная проверка всех операций ввода-вывода

### Ожидаемая оценка 8
Реализовывал функционал на 8 баллов, включая все дополнительные задачи.

### Сборка
```bash
gcc -o copy_file copy_file.c
```

### Использование
```bash
./copy_file <исходный_файл> <целевой_файл>
```

Программа корректно работает с файлами любого типа (текстовые, бинарные, исполняемые) и сохраняет исходные права доступа, обеспечивая возможность запуска исполняемых файлов после копирования.