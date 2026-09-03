# HUD-модели как world-модели

Любой физический предмет с HUD-секцией может использовать `item_visual` из HUD
как обычную модель в мире:

```ini
[wpn_example]
hud = wpn_example_hud
use_hud_model_as_world = true
hud_world_scale = 0.025

[wpn_example_hud]
item_visual = dynamics\weapons\wpn_example\wpn_example_hud
```

Если модель задаётся не через `hud`, её можно указать напрямую:

```ini
hud_world_visual = dynamics\items\example_hud
```

Приоритет масштаба: `hud_world_scale` в секции предмета, затем `world_scale` в
HUD-секции, затем совместимый параметр `world_scaling` в секции предмета.
Масштаб применяется к рендеру, spatial bounds и автоматически созданной
коллизии. Допустимый диапазон: `0.001`–`100`.

Анимированная world-копия не связывается со скелетом рук и всегда запускает
собственный цикл `idle`. Если `idle` отсутствует, движок пишет предупреждение и
оставляет bind pose.

## Аттачи оружия

Если основное оружие использует HUD-модель в мире, внешние аттачи автоматически
берут `hud_attach_visual` и HUD-параметры крепления. Это можно переопределить в
секции отдельного аттача:

```ini
[example_scope]
use_hud_model_as_world = true
hud_attach_visual = dynamics\weapons\addons\example_scope_hud
hud_attach_bone = wpn_scope
hud_attach_position = 0, 0, 0
hud_attach_rotation = 0, 0, 0
hud_attach_scale = 1
```

World-копия аттача также играет только `idle`; `hud_hand_pose` и привязка рук
для неё не выполняются.

## Автоматическая коллизия

У предметов без экспортированных OGF bone shapes движок по умолчанию создаёт
compound shell из bounding boxes отдельных render-мешей. Настройки:

```ini
auto_generate_collision = true
auto_collision_max_boxes = 16
force_auto_generated_collision = false
```

Для `use_hud_model_as_world = true` принудительная генерация включена по
умолчанию, поэтому collision-box’ы в Blender не обязательны. Максимум можно
изменять от 1 до 64; меньшие значения дешевле для ODE.
