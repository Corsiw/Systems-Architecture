#!/bin/bash

# Может быть ошибка переполнения
factorial() {
    local n=$1
    if [ $n -eq 0 ] || [ $n -eq 1 ]; then
        echo 1
    else
        local prev=$(factorial $((n - 1)))
        echo $((n * prev))
    fi
}


echo "=== ПРОГРАММА ПОДСЧЕТА ФАКТОРИАЛА ==="

# Проверка наличия аргумента
if [ $# -eq 1 ]; then
    if ! [[ $1 =~ ^[0-9]+$ ]]; then
        echo "Ошибка: Введите положительное целое число"
        exit 1
    fi
    echo "Факториал $1 = $(factorial $1)"
else
    echo "Использование: $0 <число>"
    echo "Пример: $0 10"
    exit 1
fi
