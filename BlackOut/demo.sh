#!/usr/bin/env bash
set -euo pipefail

escenario=${1:-apagon}
puerto_base=${BLACKOUT_PUERTO_BASE:-18080}
duracion=${BLACKOUT_DURACION:-30}
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
historial=()
total_cadena=
estado_independiente=
total_independiente=
cadena_cerrada=0
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
    local indice respuesta id estado ventas total referencia= coincide=1 salida_independiente
    cadena_cerrada=1
    estado_independiente=
    total_independiente=

    printf '\n[monitor %2ds] Cadena A-B-C\n' "$SECONDS"
    for indice in "${!ids[@]}"; do
        id=${ids[indice]}
        if [[ -n ${tiendas[indice]:-} ]] &&
            respuesta=$("$binario" cliente "$host" "${puertos[indice]}" ESTADO 2>/dev/null); then
            read -r _ id estado ventas total <<<"$respuesta"
            historial[indice]="$estado · ventas propias $ventas"
            if [[ $estado == CERRADA ]]; then
                : >"$temporal/detener-$id"
            fi
            if (( indice == 3 )); then
                estado_independiente=$estado
                total_independiente=$ventas
                salida_independiente="D independiente · $estado · ventas $ventas"
                continue
            fi
            printf '  %s · %s\n' "$id" "${historial[indice]}"
            historial[indice]+=" · copia de cadena $total"
            if [[ $estado != CERRADA ]]; then
                cadena_cerrada=0
            fi
            if [[ -z $referencia ]]; then
                referencia=$total
            elif [[ $total != "$referencia" ]]; then
                coincide=0
            fi
        else
            estado="$id SIN RESPUESTA · último dato: ${historial[indice]:-sin datos}"
            if (( indice == 3 )); then
                salida_independiente=$estado
                continue
            fi
            printf '  %s\n' "$estado"
            if [[ -n ${tiendas[indice]:-} ]]; then
                coincide=0
                cadena_cerrada=0
            fi
        fi
    done
    # Las tiendas activas consultadas tienen copias. Muestro un total cuando coinciden.
    total_cadena=
    if (( coincide )) && [[ -n $referencia ]]; then
        total_cadena=$referencia
        printf '  Total de la cadena %s (confirmado por tiendas activas)\n' "$total_cadena"
    else
        printf '  Total de la cadena pendiente de confirmar\n'
    fi
    printf '  %s\n' "$salida_independiente"
}

envios_confirmados() {
    local indice respuesta
    for indice in 0 1 2; do
        if [[ -z ${tiendas[indice]:-} ]]; then
            continue
        fi
        respuesta=$("$binario" cliente "$host" "${puertos[indice]}" PENDIENTES 2>/dev/null) || return 1
        [[ $respuesta == 'PENDIENTES 0' ]] || return 1
    done
}

ejecutar_comprador() {
    trap - EXIT INT TERM
    local indice=$1 monto respuesta
    local pausas=(0.3 0.5 0.7 0.9 1.1)
    local detener_tienda=$temporal/detener-${ids[indice]}
    while [[ ! -e $detener && ! -e $detener_tienda ]]; do
        # El comprador espera un rato al azar y luego elige cuánto gastar.
        sleep "${pausas[RANDOM % ${#pausas[@]}]}"
        if [[ -e $detener || -e $detener_tienda ]]; then
            break
        fi
        monto=$((7 + RANDOM % 10))
        respuesta=$("$binario" cliente "$host" "${puertos[indice]}" COMPRA "$monto" 2>/dev/null) || true
        printf '[compra  %2ds] %s · $%d.00 · %s\n' \
            "$SECONDS" "${ids[indice]}" "$monto" "${respuesta:-SIN RESPUESTA}"
        if [[ $respuesta == 'ERROR CERRADA' ]]; then
            break
        fi
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

printf 'Meta compartida de $%s para A, B y C. D usa su propia meta de $%s.\n' "${meta#\$}" "${meta#\$}"
SECONDS=0
# El & deja un comprador de Bash en segundo plano por tienda.
for indice in "${!ids[@]}"; do
    ejecutar_comprador "$indice" &
    simuladores+=("$!")
done

# Confirmo el cierre de la cadena en dos consultas seguidas antes de terminar.
cierre_anterior=
cierre_anunciado=0
while true; do
    consultar_tiendas
    if (( cadena_cerrada )) && [[ -n $total_cadena ]] && envios_confirmados; then
        if [[ $total_cadena == "$cierre_anterior" ]]; then
            if [[ $estado_independiente == CERRADA ]]; then
                printf 'La cadena cerró con %s y D cerró por separado con %s. Fin de la demo.\n' \
                    "$total_cadena" "$total_independiente"
                break
            fi
            if (( ! cierre_anunciado )); then
                printf 'La cadena cerró con %s. D sigue hasta su propia meta.\n' "$total_cadena"
                cierre_anunciado=1
            fi
        fi
        cierre_anterior=$total_cadena
    else
        cierre_anterior=
    fi
    if (( SECONDS >= duracion )); then
        printf 'Se alcanzó el límite de %s segundos antes de confirmar todos los cierres.\n' "$duracion"
        break
    fi
    if [[ $escenario == apagon && -n ${tiendas[1]:-} ]] && (( SECONDS >= instante_apagon )); then
        printf '[demo    %2ds] Se simula un apagón en B.\n' "$SECONDS"
        : >"$temporal/detener-B"
        kill -KILL "${tiendas[1]}" 2>/dev/null || true
        wait "${tiendas[1]}" 2>/dev/null || true
        unset 'tiendas[1]'
        cierre_anterior=
    fi
    esperar_segundos 1
done

: >"$detener"
for pid in "${simuladores[@]}"; do
    wait "$pid" || true
done
simuladores=()

if [[ $mostrar_registros == 1 ]]; then
    for id in "${ids[@]}"; do
        printf '\nRegistro de ejecución de %s\n' "$id"
        cat -- "$temporal/$id.salida"
    done
fi
