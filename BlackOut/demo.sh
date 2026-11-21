#!/usr/bin/env bash
set -euo pipefail

escenario=${1:-cierre}
puerto_base=${BLACKOUT_PUERTO_BASE:-18080}
rondas=${BLACKOUT_RONDAS:-12}
pausa=${BLACKOUT_PAUSA:-0.25}
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
if [[ ! $rondas =~ ^[1-9][0-9]{0,2}$ || ! $pausa =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    printf 'Usa entre 1 y 999 rondas y una pausa de cero segundos o más.\n' >&2
    exit 2
fi
if [[ $escenario == apagon ]] && (( rondas < 3 )); then
    printf 'El escenario apagon necesita al menos tres rondas.\n' >&2
    exit 2
fi

carpeta=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
temporal=$(mktemp -d)
binario=$temporal/blackout
ids=(A B C D)
puertos=("$puerto_base" "$((puerto_base + 1))" "$((puerto_base + 2))" "$((puerto_base + 3))")
pids=()
compradores=()
ultimo_estado=()

limpiar() {
    local pid
    for pid in "${compradores[@]}" "${pids[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    for pid in "${compradores[@]}" "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    rm -rf -- "$temporal"
}

trap limpiar EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

iniciar_tienda() {
    local indice=$1 meta=$2
    shift 2
    "$binario" tienda "${ids[indice]}" "${puertos[indice]}" "$meta" \
        "$temporal/${ids[indice]}.log" "$@" >"$temporal/${ids[indice]}.salida" 2>&1 &
    pids[indice]=$!
}

esperar_tienda() {
    local indice=$1 estado intento
    for ((intento = 0; intento < 40; intento++)); do
        if ! kill -0 "${pids[indice]}" 2>/dev/null; then
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

monitorear() {
    local indice estado
    for indice in "${!ids[@]}"; do
        if estado=$("$binario" cliente "$host" "${puertos[indice]}" ESTADO 2>/dev/null); then
            ultimo_estado[indice]=$estado
            printf '  %s\n' "$estado"
        elif [[ -n ${ultimo_estado[indice]:-} ]]; then
            printf '  %s SIN RESPUESTA. Último estado conocido: %s\n' "${ids[indice]}" "${ultimo_estado[indice]}"
        else
            printf '  %s SIN RESPUESTA. Aún no hay datos.\n' "${ids[indice]}"
        fi
    done
}

lanzar_compra() {
    local indice=$1 monto=$((200 + RANDOM % 250))
    "$binario" cliente "$host" "${puertos[indice]}" COMPRA "$monto" \
        >"$temporal/${ids[indice]}.compra" 2>&1 &
    compradores[indice]=$!
    printf '  Cliente → %s. Compra por %d centavos.\n' "${ids[indice]}" "$monto"
}

gcc -std=c11 -Wall -Wextra -Werror -pthread "$carpeta/blackout.c" -o "$binario"

meta_cadena=1800
meta_independiente=1500
if [[ $escenario == apagon ]]; then
    meta_cadena=1000000
    meta_independiente=1000000
fi

iniciar_tienda 0 "$meta_cadena" "$host:${puertos[1]}" "$host:${puertos[2]}"
iniciar_tienda 1 "$meta_cadena" "$host:${puertos[0]}" "$host:${puertos[2]}"
iniciar_tienda 2 "$meta_cadena" "$host:${puertos[0]}" "$host:${puertos[1]}"
iniciar_tienda 3 "$meta_independiente"
for indice in "${!ids[@]}"; do
    esperar_tienda "$indice"
done

printf 'Cadena A, B y C con meta de %d centavos. D es independiente y su meta es %d.\n' \
    "$meta_cadena" "$meta_independiente"
printf 'Estado inicial (tienda, estado, ventas propias, total en centavos)\n'
monitorear

for ((ronda = 1; ronda <= rondas; ronda++)); do
    printf '\nRonda %d\n' "$ronda"
    if [[ $escenario == apagon ]] && (( ronda == 3 )); then
        printf 'Se simula un apagón en B.\n'
        kill -KILL "${pids[1]}"
        wait "${pids[1]}" 2>/dev/null || true
        unset 'pids[1]'
    fi

    if [[ $escenario == apagon ]] && (( ronda >= 3 )); then
        lanzar_compra 0
        lanzar_compra 2
    else
        lanzar_compra "$(( (ronda - 1) % 3 ))"
        lanzar_compra "$(( ronda % 3 ))"
    fi
    lanzar_compra 3

    for indice in "${!compradores[@]}"; do
        wait "${compradores[indice]}" || true
        respuesta=$(<"$temporal/${ids[indice]}.compra")
        printf '  Respuesta de %s: %s\n' "${ids[indice]}" "${respuesta:-sin respuesta}"
    done
    compradores=()
    monitorear
    sleep "$pausa"
done

sleep 0.1
printf '\nEstado final tras dar tiempo a la difusión\n'
monitorear
