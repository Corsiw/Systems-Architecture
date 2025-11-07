#!/bin/bash

echo "=== Программа проверки на четность ==="

read -p "Введите целое число: " number

if ! [[ "$number" =~ ^-?[0-9]+$ ]]; then
    echo "Ошибка: введите корректное целое число!"
    exit 1
fi

if [ $((number % 2)) -eq 0 ]; then
    echo "Число $number чётное"
else
    echo "Число $number нечётное"
fi