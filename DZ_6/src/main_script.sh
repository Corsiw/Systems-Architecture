#!/bin/bash

# Главный скрипт, который вызывает другие скрипты

echo "=== ГЛАВНЫЙ СКРИПТ ==="
echo "Текущая дата: $(date)"

# Получаем абсолютный путь до main_script.sh
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "1. Запуск script1.sh:"
"$SCRIPT_DIR/script1.sh"

echo "2. Запуск script2.sh:"
"$SCRIPT_DIR/script2.sh" 'Hello world'

echo "=== КОНЕЦ ГЛАВНОГО СКРИПТА ==="