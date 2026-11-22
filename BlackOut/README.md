# BlackOut

A, B y C son tiendas de una misma cadena y juntas tienen una meta de $100. D lleva su propia cuenta.

## Probar la demo en Linux

Extrae el ZIP, abre una terminal en la carpeta `BlackOut` y ejecuta:

```bash
bash demo.sh
```

El script compila con GCC, inicia las cuatro tiendas y muestra las compras y el monitor en esa misma terminal. En Debian o Ubuntu, si hace falta el compilador, instala `build-essential` con `sudo apt update` y `sudo apt install build-essential`.

En la demo se apaga B. Su línea queda como `SIN RESPUESTA` y muestra el último dato que vio el monitor. A y C siguen vendiendo. El monitor muestra un solo total cuando las tiendas activas coinciden. D cierra cuando alcanza su meta por separado. Los montos cambian en cada corrida porque las compras son al azar. La salida termina con `Fin de la demo` cuando se confirman los cierres.

## De dónde salen las compras

La guía menciona un generador que se entregaría aparte. Esta demo genera las compras en `demo.sh`. La función `ejecutar_comprador()` corre en segundo plano una vez por tienda. Usa `RANDOM` primero para elegir cuánto esperar y después para elegir un monto de $7 a $16. Entonces llama a `blackout cliente` para mandar la compra por TCP.

Los compradores de la demo corren como procesos de Bash. Dentro de una tienda, los hilos de C atienden los pedidos y comparten las ventas con otros locales. La demo elige dólares completos para que se vea fácil. El programa guarda centavos enteros, así que una compra de $7.50 se guarda como 750 y se puede sumar con exactitud.

## Pruebas opcionales

- `bash demo.sh cierre` muestra el cierre con las tres tiendas de la cadena en línea.
- `bash probar_recuperacion.sh` prueba el regreso de B con el mismo registro. Uno de los casos deja $8.36 guardados solo en B. B queda en `RECUPERANDO` si reinicia sola. A y C cierran con $105.27 y, cuando B vuelve a conectarse, las tres muestran $113.63.

### Ajustes de la demo

| Variable | Valor por defecto | Para qué sirve |
| --- | --- | --- |
| `BLACKOUT_PUERTO_BASE` | `18080` | Primer puerto de cuatro consecutivos en la demo. |
| `BLACKOUT_DURACION` | `30` | Tiempo máximo en segundos. |
| `BLACKOUT_META` | `100.00` | Meta en dólares de la cadena y meta separada de D. |
| `BLACKOUT_APAGON_EN` | `1` | Segundo en que se apaga B. |
| `BLACKOUT_MOSTRAR_REGISTROS` | `0` | Con `1` muestra conexiones y ventas registradas al final. |

Por ejemplo, para simular el apagón en el segundo 2 y ver los registros:

```bash
BLACKOUT_APAGON_EN=2 BLACKOUT_MOSTRAR_REGISTROS=1 bash demo.sh
```

## Ejecución manual opcional

Para iniciar las tiendas una por una, compila en la carpeta `BlackOut`.

```bash
gcc -std=c11 -Wall -Wextra -Werror -pthread blackout.c -o blackout
```

El modo `tienda` recibe el ID, un puerto libre, la meta en dólares, el archivo de ventas y las direcciones de sus pares. Usa un ID distinto para cada tienda, de hasta 31 caracteres, con letras, números, guion o guion bajo. Cada proceso necesita su propio archivo de ventas.

En una terminal, inicia A con la dirección de B.

```bash
./blackout tienda A 18080 100.00 ventas-A.log 127.0.0.1:18081
```

En otra terminal, inicia B con la dirección de A.

```bash
./blackout tienda B 18081 100.00 ventas-B.log 127.0.0.1:18080
```

El modo `cliente` recibe el host, el puerto y la solicitud. En una tercera terminal, registra $7.50 en A y consulta las dos tiendas.

```bash
./blackout cliente 127.0.0.1 18080 COMPRA 7.50
./blackout cliente 127.0.0.1 18080 ESTADO
./blackout cliente 127.0.0.1 18081 ESTADO
```

Los últimos dos campos de `ESTADO` son las ventas propias y el total. Con registros vacíos, A muestra `ESTADO A ABIERTA $7.50 $7.50`. B muestra `ESTADO B ABIERTA $0.00 $7.50` después de recibir la venta. Usa importes positivos de hasta dos decimales. Si escribes el signo de dólar, usa comillas simples, como en `COMPRA '$7.50'`.

Inicia una tienda independiente dejando el comando hasta el archivo de ventas. Para cambiar los pares de una tienda de cadena mientras se ejecuta, pasa `@pares-A.txt` en lugar de las direcciones. El archivo admite un `HOST:PUERTO` por línea, líneas vacías y comentarios con `#`. La tienda lo vuelve a leer aproximadamente cada segundo.

Para probar el reinicio manualmente, apaga B y vuelve a usar su comando con el mismo ID, puerto y archivo de ventas. Deja A en ejecución para que B pueda recuperar lo que le falta.

## El recorrido de una venta

1. **Recibir.** En `atender()` llega el pedido `COMPRA` por TCP. La tienda revisa el monto y si aún puede vender. En `red.h` están las funciones que abren el socket y leen los mensajes.
2. **Guardar.** En `guardar()` se comprueba el identificador de la venta, se guarda en el archivo con `fsync()` y se suma el monto. Bakery da un turno a cada hilo para que solo uno cambie el registro a la vez. Si el disco tarda, los otros hilos esperan ese turno. La espera por la red queda fuera. Si dos hilos toman el mismo boleto, va primero el de menor índice. Al salir, deja su boleto en cero.
3. **Compartir.** En `publicar()`, el hilo toma una copia de la venta, suelta el turno y la envía por TCP. Cada origen envía a sus pares en el orden de conexión y espera cada confirmación o su tiempo límite. Si varias tiendas venden a la vez, sus avisos pueden llegar intercalados. Cada tienda suma lo que recibe. El origen y el número de venta permiten reconocer y descartar duplicados.
4. **Recuperar.** Al reiniciar con su archivo, la tienda consulta a los pares que responden. Para volver a aceptar compras necesita leer el registro anunciado de al menos uno y terminar la lectura de los demás que respondieron en esa vuelta. Si nadie responde o se corta una lectura, sigue en `RECUPERANDO` y reintenta. Las copias que quedaron en equipos aislados se incorporan cuando esos equipos vuelven.

Cada tienda de cadena tiene dos hilos para atender solicitudes y otro para publicar. La tienda deja de aceptar compras cuando su copia del total llega a la meta. Si va en $95 y entra una compra de $14, registra la compra completa y cierra con $109. En la cadena también pueden llegar ventas pendientes de otras tiendas. La demo simula el apagón con `SIGKILL` y permite terminar los envíos entre las tiendas activas.
