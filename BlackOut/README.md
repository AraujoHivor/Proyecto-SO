# BlackOut

## Demo en Linux

Extrae el ZIP y abre una terminal en la carpeta `BlackOut`. Para ejecutar la demostración completa, usa:

```bash
bash demo.sh
```

El comando compila el programa con GCC, inicia las tiendas, genera las compras y muestra el monitor en la misma terminal. Al terminar, detiene los procesos y borra sus archivos temporales.

A, B y C comparten una meta de **$100.00**. D lleva su cuenta aparte. Durante la demo se simula un apagón en B y el monitor muestra el total que confirman las tiendas activas.

La demo usa Bash y GCC. Si falta GCC en Debian o Ubuntu, se puede instalar con `sudo apt update` y `sudo apt install build-essential`.

## Pruebas y ajustes opcionales

- `bash demo.sh cierre` ejecuta la demo con las tres tiendas de la cadena en línea.
- `bash probar_recuperacion.sh` reinicia B con el mismo registro y verifica con montos fijos qué pasa antes y después del cierre de la cadena.

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

## Cómo funciona

- **Procesos y sockets.** Cada tienda es un proceso. Las tiendas y sus clientes se conectan por TCP. Las tiendas se mandan ventas individuales, con tienda de origen, número y monto. Cada tienda suma sus copias en centavos enteros.
- **Hilos y orden.** Dos hilos atienden solicitudes y otro publica en las tiendas de cadena. La tienda de origen guarda la venta primero. Para cada tienda de origen, el publicador intenta enviar a sus pares en el orden en que se conectaron. Espera la confirmación de cada uno o el tiempo límite antes de pasar al siguiente. Si un par vuelve, queda al final del orden activo.
- **Bakery de Lamport.** Cada hilo elige un boleto mayor que los que ve y espera a que le toque. Si dos eligen el mismo número, va primero el hilo de menor índice. Al salir deja su boleto en cero. La espera por la red queda fuera del turno. El turno cubre la escritura con `fsync()` y la actualización del total. `OK REGISTRADA` se envía después de soltar el turno.
- **Apagón y recuperación.** El apagón se simula con `SIGKILL`. Al volver, la tienda carga su archivo y consulta ventas de los pares que responden. Puede aparecer como `RECUPERANDO` y rechazar compras mientras se pone al día. El origen y el número identifican cada venta y evitan sumarla dos veces. Si una venta quedó guardada solo en el equipo apagado, se incorpora cuando ese equipo vuelve con su archivo.
- **Cierre.** Cada tienda deja de aceptar compras cuando su copia del total alcanza la meta. Las ventas ya aceptadas pueden llevar el total por encima de $100.00. El proceso puede responder a consultas y recibir ventas pendientes hasta que termina la demo. `PENDIENTES 0` se refiere a los pares que responden.
