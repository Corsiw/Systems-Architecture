#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIRECTORY "files"
#define BASE_FILE "a"

int main() {
  int depth = 1;
  char prev[256], curr[256];
  int fd;

  if (mkdir(DIRECTORY, 0755) == -1 && errno != EEXIST) {
    perror("mkdir");
    return 1;
  }

  if (chdir(DIRECTORY) == -1) {
    perror("chdir");
    return 1;
  }

  fd = open(BASE_FILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd == -1) {
    perror("open base file");
    return 1;
  }
  close(fd);

  strcpy(prev, BASE_FILE);

  while (1) {
    snprintf(curr, sizeof(curr), "a%d", depth + 1);

    if (symlink(prev, curr) == -1) {
      perror("symlink");
      break;
    }

    fd = open(curr, O_RDONLY);
    if (fd == -1) {
      printf("Ошибка открытия на глубине %d\n", depth + 1);
      printf("errno = %d (%s)\n", errno, strerror(errno));
      break;
    }

    close(fd);
    strlcpy(prev, curr, 256);
    depth++;
  }

  printf("Максимальная глубина рекурсии символьных ссылок: %d\n", depth);

  return 0;
}
