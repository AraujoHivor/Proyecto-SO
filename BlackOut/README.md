# BlackOut

## Ejecutar en WSL

Extrae el ZIP, abre Ubuntu y entra a la carpeta que contiene `demo.sh`. Por ejemplo, si lo extrajiste en Descargas de Windows, cambia el usuario y el nombre de la carpeta por los tuyos.

```bash
cd "/mnt/c/Users/TU_USUARIO/Downloads/BlackOut-referencia/BlackOut"
bash demo.sh
```

Si te falta GCC, instala las herramientas con `sudo apt update` y `sudo apt install build-essential`. Si ya tienes una copia de GitHub, puedes actualizarla con `git pull --ff-only`.

A, B y C comparten una sola meta de **$100.00**. Cada tienda guarda sus propias ventas y una copia del total de la cadena. D lleva su cuenta aparte y tiene su propia meta. La demo genera compras aleatorias de $7.00 a $16.00, simula un apagón en B y sigue mostrando el total cuando las tiendas activas coinciden. Termina cuando las tiendas activas y D cierran y se procesan los envíos pendientes entre las tiendas activas.

```bash
bash demo.sh cierre            # Las tres tiendas quedan en línea
bash demo.sh apagon            # Mismo escenario que bash demo.sh
bash probar_recuperacion.sh    # Prueba de reinicio con montos fijos
```

La prueba de recuperación reinicia B con el mismo registro. Comprueba que B sigue vendiendo si el total todavía está por debajo de la meta y que rechaza compras si la cadena ya cerró. También prueba una venta de $8.36 que quedó guardada solo en B. A y C cierran con $105.27 y, cuando B vuelve, las tres quedan cerradas con $113.63. Los dos scripts cierran sus procesos y borran los archivos temporales al salir.

## Opciones de la demo

| Variable | Valor por defecto | Para qué sirve |
| --- | --- | --- |
| `BLACKOUT_PUERTO_BASE` | `18080` | Primer puerto de cuatro consecutivos. |
| `BLACKOUT_DURACION` | `30` | Tiempo máximo en segundos. |
| `BLACKOUT_META` | `100.00` | Meta en dólares de la cadena y meta separada de D. |
| `BLACKOUT_APAGON_EN` | `1` | Segundo en que se apaga B. |
| `BLACKOUT_MOSTRAR_REGISTROS` | `0` | Con `1` muestra conexiones y ventas registradas al final. |

```bash
BLACKOUT_APAGON_EN=2 BLACKOUT_MOSTRAR_REGISTROS=1 bash demo.sh
BLACKOUT_PUERTO_BASE=19100 bash probar_recuperacion.sh
```

## Usar el programa a mano

Compila en la carpeta `BlackOut`.

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
