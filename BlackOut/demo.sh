#!/usr/bin/env bash
set -euo pipefail

escenario=${1:-cierre}
puerto_base=${BLACKOUT_PUERTO_BASE:-18080}
duracion=${BLACKOUT_DURACION:-18}
meta=${BLACKOUT_META:-100.00}
instante_apagon=${BLACKOUT_APAGON_EN:-1}
mostrar_registros=${BLACKOUT_MOSTRAR_REGISTROS:-0}
host=127.0.0.1

if [[ $# -gt 1 || ( $escenario != cierre && $escenario != apagon ) ]]; then
    printf 'Uso: %s [cierre|apagon]\n' "$0" >&2
    exit 2
fi
if [[ ! $puerto_base =~ ^[1-9][0-9]{0,4}$ ]] ||
    (( puerto_base < 1024 || puerto_base > 65532 )); then
    printf 'BLACKOUT_PUERTO_BASE debe estar entre 1024 y 65532.\n' >&2
    exit 2
fi
if [[ ! $duracion =~ ^[1-9][0-9]{0,5}$ ]]; then
    printf 'BLACKOUT_DURACION debe ser un número entero positivo de segundos.\n' >&2
    exit 2
fi
if [[ ! $meta =~ ^\$?[0-9]+([.][0-9]{1,2})?$ || ${meta#\$} =~ ^0+([.]0+)?$ ]]; then
    printf 'BLACKOUT_META debe ser un importe positivo en dólares, por ejemplo 100.00.\n' >&2
    exit 2
fi
if [[ $escenario == apagon ]]; then
    if [[ ! $instante_apagon =~ ^[1-9][0-9]{0,5}$ ]] ||
        (( instante_apagon >= duracion )); then
        printf 'BLACKOUT_APAGON_EN debe indicar un segundo anterior al fin de la demo.\n' >&2
        exit 2
    fi
fi

carpeta=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
temporal=$(mktemp -d)
detener=$temporal/detener
binario=$temporal/blackout
ids=(A B C D)
puertos=("$puerto_base" "$((puerto_base + 1))" "$((puerto_base + 2))" "$((puerto_base + 3))")
tiendas=()
simuladores=()
temporizador=

limpiar() {
    local pid
    : >"$detener"
    if [[ -n $temporizador ]]; then
        kill "$temporizador" 2>/dev/null || true
        wait "$temporizador" 2>/dev/null || true
    fi
    for pid in "${tiendas[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    for pid in "${simuladores[@]}" "${tiendas[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    rm -rf -- "$temporal"
}

trap limpiar EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

esperar_segundos() {
    sleep "$1" &
    temporizador=$!
    wait "$temporizador"
    temporizador=
}

iniciar_tienda() {
    local indice=$1
    shift
    "$binario" tienda "${ids[indice]}" "${puertos[indice]}" "$meta" \
        "$temporal/${ids[indice]}.log" "$@" >"$temporal/${ids[indice]}.salida" 2>&1 &
    tiendas[indice]=$!
}

esperar_tienda() {
    local indice=$1 estado intento
    for ((intento = 0; intento < 40; intento++)); do
        if ! kill -0 "${tiendas[indice]}" 2>/dev/null; then
            break
        fi
        if estado=$("$binario" cliente "$host" "${puertos[indice]}" ESTADO 2>/dev/null) &&
            [[ $estado == "ESTADO ${ids[indice]} ABIERTA "* ]]; then
            return 0
        fi
        sleep 0.05
    done
    printf 'La tienda %s no inició en el puerto %s.\n' "${ids[indice]}" "${puertos[indice]}" >&2
    cat -- "$temporal/${ids[indice]}.salida" >&2
    return 1
}

consultar_tiendas() {
    local indice estado archivo
    for indice in "${!ids[@]}"; do
        archivo=$temporal/${ids[indice]}.estado
        if estado=$("$binario" cliente "$host" "${puertos[indice]}" ESTADO 2>/dev/null); then
            printf '%s\n' "$estado" >"$archivo"
        elif [[ -s $archivo ]]; then
            estado="${ids[indice]} SIN RESPUESTA. Último estado conocido: $(<"$archivo")"
        else
            estado="${ids[indice]} SIN RESPUESTA. Aún no hay datos."
        fi
        printf '[monitor %2ds] %s\n' "$SECONDS" "$estado"
    done
}

ejecutar_monitor() {
    trap - EXIT INT TERM
    while sleep 1 && [[ ! -e $detener ]]; do
        consultar_tiendas
    done
}

ejecutar_comprador() {
    trap - EXIT INT TERM
    local indice=$1 demora pausa centavos monto respuesta
    while [[ ! -e $detener ]]; do
        demora=$((300 + RANDOM % 801))
        printf -v pausa '%d.%03d' "$((demora / 1000))" "$((demora % 1000))"
        sleep "$pausa"
        if [[ -e $detener ]]; then
            break
        fi
        centavos=$((700 + RANDOM % 901))
        printf -v monto '%d.%02d' "$((centavos / 100))" "$((centavos % 100))"
        respuesta=$("$binario" cliente "$host" "${puertos[indice]}" COMPRA "$monto" 2>/dev/null) || true
        printf '[compra  %2ds] %s · $%s · %s\n' \
            "$SECONDS" "${ids[indice]}" "$monto" "${respuesta:-SIN RESPUESTA}"
    done
}

gcc -std=c11 -Wall -Wextra -Werror -pthread "$carpeta/blackout.c" -o "$binario"

iniciar_tienda 0 "$host:${puertos[1]}" "$host:${puertos[2]}"
iniciar_tienda 1 "$host:${puertos[0]}" "$host:${puertos[2]}"
iniciar_tienda 2 "$host:${puertos[0]}" "$host:${puertos[1]}"
iniciar_tienda 3
for indice in "${!ids[@]}"; do
    esperar_tienda "$indice"
done

printf 'Meta de $%s para la cadena A-B-C y para la tienda independiente D.\n' "${meta#\$}"
printf 'El monitor muestra tienda, estado, ventas propias y total en dólares.\n'
SECONDS=0
consultar_tiendas
for indice in "${!ids[@]}"; do
    ejecutar_comprador "$indice" &
    simuladores+=("$!")
done
ejecutar_monitor &
simuladores+=("$!")

if [[ $escenario == apagon ]]; then
    esperar_segundos "$instante_apagon"
    printf '[demo    %2ds] Se simula un apagón en B.\n' "$SECONDS"
    kill -KILL "${tiendas[1]}"
    wait "${tiendas[1]}" 2>/dev/null || true
    unset 'tiendas[1]'
    esperar_segundos "$((duracion - instante_apagon))"
else
    esperar_segundos "$duracion"
fi

: >"$detener"
for pid in "${simuladores[@]}"; do
    wait "$pid" || true
done
simuladores=()
sleep 0.3
printf '\nÚltima consulta tras detener las compras\n'
consultar_tiendas

if [[ $mostrar_registros == 1 ]]; then
    for id in "${ids[@]}"; do
        printf '\nRegistro de ejecución de %s\n' "$id"
        cat -- "$temporal/$id.salida"
    done
fi
