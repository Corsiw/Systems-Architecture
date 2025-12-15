#!/bin/bash

DIRECTORY="files"
BASE_FILE="a"

# Создаём тестовую директорию
mkdir -p "$DIRECTORY" || {
  echo "Не удалось создать директорию"
  exit 1
}

cd "$DIRECTORY" || exit 1

# Создаём обычный файл "a"
> "$BASE_FILE" || {
  echo "Не удалось создать базовый файл"
  exit 1
}

prev="$BASE_FILE"
depth=1

while true; do
  curr="a$((depth + 1))"

  # Создаём символьную ссылку
  ln -s "$prev" "$curr" 2>/dev/null
  if [ $? -ne 0 ]; then
    echo "Ошибка создания символьной ссылки на глубине $((depth + 1))"
    break
  fi

  # Пытаемся открыть файл
  if ! exec {fd}<"$curr" 2>/dev/null; then
    echo "Ошибка открытия файла на глубине $((depth + 1))"
    break
  fi

  # Закрываем файловый дескриптор
  exec {fd}<&-

  prev="$curr"
  depth=$((depth + 1))
done

echo "Максимальная глубина рекурсии символьных ссылок: $depth"
