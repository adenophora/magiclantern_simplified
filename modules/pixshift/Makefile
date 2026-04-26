# Makefile for pixshift Magic Lantern module
#
# Структура идентична остальным модулям (intervalometer, dual_iso и т.д.):
# ML_PATH указывает на корень репозитория magiclantern_simplified.
#
# Сборка:
#   cd modules/pixshift
#   make ML_PATH=../../
#
# Для конкретной камеры (например 50D):
#   make ML_PATH=../../ CAMERA=50D.109

MODULE_NAME     = pixshift
MODULE_OBJS     = pixshift.o

# Путь к корню ML-репозитория (переопределяется из командной строки)
ML_PATH        ?= ../../

include $(ML_PATH)/modules/Makefile.module
