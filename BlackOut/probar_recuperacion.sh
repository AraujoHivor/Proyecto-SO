#!/usr/bin/env bash
set -euo pipefail

puerto_base=${BLACKOUT_PUERTO_BASE:-19080}
if [[ ! $puerto_base =~ ^[1-9][0-9]{0,4}$ ]] ||
    (( puerto_base < 1024 || puerto_base > 65533 )); then
    printf 'BLACKOUT_PUERTO_BASE debe estar entre 1024 y 65533.\n' >&2
    exit 2
fi

carpeta=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
temporal=$(mktemp -d)
binario=$temporal/blackout
host=127.0.0.1
declare -A puertos=([A]="$puerto_base" [B]="$((puerto_base + 1))" [C]="$((puerto_base + 2))")
declare -A procesos=()

limpiar() {
    local pid
    for pid in "${procesos[@]}"; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    for pid in "${procesos[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    rm -rf -- "$temporal"
}

trap limpiar EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

fallar() {
    local tienda
    printf 'Prueba fallida. %s\n' "$1" >&2
    for tienda in A B C; do
        if [[ -s $temporal/$tienda.salida ]]; then
            printf '\nRegistro reciente de %s\n' "$tienda" >&2
            tail -n 12 -- "$temporal/$tienda.salida" >&2
        fi
    done
    exit 1
}

iniciar() {
    local tienda=$1 par
    shift
    local pares=()
    for par in "$@"; do
        pares+=("$host:${puertos[$par]}")
    done
    "$binario" tienda "$tienda" "${puertos[$tienda]}" 100.00 "$temporal/$tienda.log" \
        "${pares[@]}" >>"$temporal/$tienda.salida" 2>&1 &
    procesos[$tienda]=$!
}

cliente() {
    local tienda=$1
    shift
    timeout --kill-after=1s 2s "$binario" cliente "$host" "${puertos[$tienda]}" "$@"
}

esperar_estado() {
    local tienda=$1 esperado="ESTADO $1 $2" respuesta= limite=$((SECONDS + 8))
    while (( SECONDS < limite )); do
        kill -0 "${procesos[$tienda]}" 2>/dev/null || fallar "La tienda $tienda dejó de ejecutarse."
        respuesta=$(cliente "$tienda" ESTADO) || true
        if [[ $respuesta == "$esperado" ]]; then
            printf '  %s\n' "$respuesta"
            return
        fi
        sleep 0.05
    done
    fallar "Esperaba $esperado. Última respuesta: ${respuesta:-sin respuesta}."
}

comprar() {
    local tienda=$1 importe=$2 esperado=${3:-OK REGISTRADA} respuesta
    respuesta=$(cliente "$tienda" COMPRA "$importe") || true
    [[ $respuesta == "$esperado" ]] ||
        fallar "Compra de \$$importe en $tienda. Esperaba $esperado y recibí ${respuesta:-sin respuesta}."
    printf '  Compra en %s por $%s · %s\n' "$tienda" "$importe" "$respuesta"
}

apagar_b() {
    kill -KILL "${procesos[B]}" || fallar 'No se pudo simular el apagón de B.'
    wait "${procesos[B]}" 2>/dev/null || true
    unset 'procesos[B]'
    if cliente B ESTADO >/dev/null; then
        fallar 'B sigue respondiendo después del apagón.'
    fi
}

comprobar_arranque_cerrado() {
    local respuesta limite=$((SECONDS + 8))
    while (( SECONDS < limite )); do
        respuesta=$(cliente B COMPRA 1.00) || true
        case "$respuesta" in
            'ERROR RECUPERANDO'|'ERROR CERRADA')
                printf '  Compra al arrancar B · %s\n' "$respuesta"
                return
                ;;
            '') sleep 0.05 ;;
            *) fallar "B recibió una compra mientras volvía a una cadena cerrada: $respuesta." ;;
        esac
    done
    fallar 'B quedó sin respuesta tras el segundo reinicio.'
}

gcc -std=c11 -Wall -Wextra -Werror -pthread "$carpeta/blackout.c" -o "$binario"
iniciar A B C
iniciar B A C
iniciar C A B

printf 'Inicio y venta propia de B\n'
esperar_estado A 'ABIERTA $0.00 $0.00'
esperar_estado B 'ABIERTA $0.00 $0.00'
esperar_estado C 'ABIERTA $0.00 $0.00'
comprar B 20.00
esperar_estado A 'ABIERTA $0.00 $20.00'
esperar_estado B 'ABIERTA $20.00 $20.00'
esperar_estado C 'ABIERTA $0.00 $20.00'

printf '\nPrimer apagón de B y recuperación antes de la meta\n'
apagar_b
comprar A 25.00
comprar C 25.00
esperar_estado A 'ABIERTA $25.00 $70.00'
esperar_estado C 'ABIERTA $25.00 $70.00'
iniciar B A C
esperar_estado B 'ABIERTA $20.00 $70.00'
comprar B 10.00
esperar_estado A 'ABIERTA $25.00 $80.00'
esperar_estado B 'ABIERTA $30.00 $80.00'
esperar_estado C 'ABIERTA $25.00 $80.00'

printf '\nSegundo apagón de B y recuperación después de la meta\n'
apagar_b
comprar A 10.00
comprar C 10.00
esperar_estado A 'CERRADA $35.00 $100.00'
esperar_estado C 'CERRADA $35.00 $100.00'
iniciar B A C
comprobar_arranque_cerrado
esperar_estado B 'CERRADA $30.00 $100.00'
comprar B 1.00 'ERROR CERRADA'
esperar_estado B 'CERRADA $30.00 $100.00'

printf '\nPrueba de recuperación superada.\n'
