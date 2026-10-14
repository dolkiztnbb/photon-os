[BITS 32]
section .text

global mt_switch_context

; Прототип для C: 
; void mt_switch_context(uint32_t *old_esp, uint32_t new_esp, uint32_t new_cr3);

mt_switch_context:
    pushfd
    pushad

    mov eax, [esp + 40]   ; 1-й аргумент: &old_task->esp
    mov [eax], esp        ; Сохраняем ESP текущей задачи

    mov ecx, [esp + 48]   ; 3-й аргумент: new_cr3 (сохраняем в ECX ДО смены ESP!)
    mov esp, [esp + 44]   ; 2-й аргумент: new_esp (переходим на стек новой задачи)

    ; --- Переключение CR3 ---
    test ecx, ecx         ; Проверяем, передан ли new_cr3 != 0
    jz .skip_cr3          ; Если new_cr3 == 0 (задача ядра), пропускаем

    mov eax, cr3          ; Читаем текущий CR3
    cmp eax, ecx          ; Сравниваем с новым CR3
    je .skip_cr3          ; Если они совпадают, пропускаем (избегаем лишнего сброса TLB)

    mov cr3, ecx          ; Загружаем новый CR3 в регистр (смена виртуального пространства!)

.skip_cr3:
    popad
    popfd
    ret