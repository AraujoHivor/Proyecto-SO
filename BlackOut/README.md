# BlackOut · referencia de estudio

Esta referencia se preparó con IA para practicar los conceptos del proyecto. La rúbrica del curso excluye el código generado por IA de la entrega.

## Ejecutar desde el ZIP

Extrae el ZIP, abre una terminal en la carpeta `BlackOut` y ejecuta:

```bash
bash demo.sh
```

En Ubuntu o WSL, la demo usa Bash y GCC. Si te falta el compilador, instala `build-essential` con `sudo apt install build-essential`. También puedes usar una copia de GitHub. Si ya la clonaste, ejecuta `git pull --ff-only` en la carpeta antes de correr el script.

El script compila con GCC, abre las tiendas y arranca los compradores. A, B y C pertenecen a la misma cadena. **Entre las tres deben alcanzar $100.00 en total.** Si A vendió $40, B $35 y C $25, el total de la cadena es $100. Cada tienda guarda una copia de ese mismo total. D es independiente: usa sus propias ventas para llegar a su meta.

Cada tienda recibe compradores por separado. Para cada compra, el simulador elige un tiempo de espera de 0,30 a 1,10 segundos y un monto de $7.00 a $16.00. El enunciado menciona un generador que se entregaría aparte. La demo incluye uno sencillo para poder correr sin ese archivo.

En la ejecución normal se simula un apagón en B. A y C pueden seguir vendiendo hasta que conocen que la cadena alcanzó la meta. El monitor muestra las ventas propias por tienda y un solo total de la cadena cuando las copias de las tiendas activas coinciden. D continúa hasta alcanzar su meta por separado. El script termina los procesos cuando observa los cierres y confirma que se procesaron los envíos pendientes entre las tiendas activas.

Para repetir la prueba con las tres tiendas en línea, usa `bash demo.sh cierre`. El comando `bash demo.sh apagon` repite el mismo escenario que se ejecuta por defecto. Los escenarios son opciones del mismo script.

La demo usa cuatro puertos desde el 18080, espera como máximo 30 segundos y simula el apagón al primer segundo. Al salir libera los procesos y borra sus archivos temporales. Puedes cambiar estos parámetros. Con `BLACKOUT_MOSTRAR_REGISTROS=1` puedes ver el orden de conexión y las ventas registradas.

```bash
BLACKOUT_PUERTO_BASE=18100 BLACKOUT_DURACION=25 BLACKOUT_META=100.00 \
  BLACKOUT_APAGON_EN=2 BLACKOUT_MOSTRAR_REGISTROS=1 bash demo.sh
```

## Compilar y consultar a mano

El siguiente comando crea el ejecutable `blackout`. El mismo ejecutable tiene dos modos, `tienda` y `cliente`.

```bash
gcc -std=c11 -Wall -Wextra -Werror -pthread blackout.c -o blackout
```

Una tienda recibe un ID único de hasta 31 caracteres (letras, números, guion o guion bajo), un puerto entre 1 y 65535, una meta en dólares y la ruta de su registro. Usa un puerto libre y un registro distinto para cada proceso. Una compra válida tiene un importe positivo de hasta dos decimales. Una solicitud inválida se rechaza y la tienda puede seguir recibiendo otras.

En una terminal, inicia A con B como única par, una meta de $100.00 y el archivo `ventas-A.log`.

```bash
./blackout tienda A 18080 100.00 ventas-A.log 127.0.0.1:18081
```

En otra terminal, inicia B con la misma meta y la dirección de A.

```bash
./blackout tienda B 18081 100.00 ventas-B.log 127.0.0.1:18080
```

En una tercera terminal, registra una compra de $7.50 en A y consulta ambas tiendas.

```bash
./blackout cliente 127.0.0.1 18080 COMPRA 7.50
./blackout cliente 127.0.0.1 18080 ESTADO
./blackout cliente 127.0.0.1 18081 ESTADO
```

Si ambas empezaron en cero, A muestra `ESTADO A ABIERTA $7.50 $7.50`. Cuando B recibe la venta, muestra `ESTADO B ABIERTA $0.00 $7.50`. Los dos últimos campos son las ventas propias y el total de la cadena. Si B aún muestra $0.00 en ambos campos, consulta de nuevo tras un momento. Los importes por consola admiten hasta dos decimales. Si escribes el signo de dólar, usa comillas simples, por ejemplo `COMPRA '$7.50'`. El programa guarda centavos enteros internamente para sumar con exactitud.

Omite las direcciones de pares para iniciar una tienda independiente. Usa direcciones alcanzables entre las tiendas si las ejecutas en equipos distintos. Para modificar la lista de pares durante la ejecución, usa un archivo precedido por `@`.

```bash
printf '%s\n' '127.0.0.1:18081' > pares-A.txt
./blackout tienda A 18080 100.00 ventas-A.log @pares-A.txt
```

El archivo admite un `HOST:PUERTO` por línea, líneas vacías y comentarios que empiezan con `#`. La tienda revisa el archivo aproximadamente cada segundo. Un archivo vacío permite arrancar una tienda de cadena antes de agregar sus pares.

## Recuperar una tienda después del corte

El escenario predeterminado mantiene a B apagada hasta el final. Para verificar un regreso sin depender de las compras aleatorias, usa:

```bash
bash probar_recuperacion.sh
```

Esta prueba comprueba dos reinicios con el mismo archivo. Primero B conserva sus $20.00 de ventas propias, recupera las ventas pendientes de A y C y acepta una compra nueva. Después, la cadena alcanza $100.00 mientras B está fuera. Al volver, B se pone al día y rechaza una compra. La prueba usa registros temporales y los borra al terminar.

En la demo predeterminada B permanece apagada hasta que termina esa corrida. Para probar un reinicio, inicia las tiendas a mano como en el ejemplo anterior. Detén B, deja a A ejecutándose y vuelve a lanzar B con el mismo identificador, puerto y archivo `ventas-B.log`. Guarda el archivo entre los dos arranques.

B recupera sus propias ventas del archivo y consulta ventas individuales en las tiendas que responden. Mientras las carga, el estado puede aparecer como `RECUPERANDO` y B rechaza compras nuevas. Si el total que recuperó sigue por debajo de $100, vuelve a atender. Si la cadena ya alcanzó la meta según los registros disponibles, B queda cerrada. Si ninguna otra tienda responde, B solo dispone de su copia local hasta que se restablezca la conexión.

Para darle a B más tiempo para recibir compradores antes del apagón simulado, ejecuta `BLACKOUT_APAGON_EN=2 bash demo.sh`. El momento de cada compra sigue siendo aleatorio. Con `bash demo.sh cierre` puedes hacer la prueba con B en línea.

## Cómo funciona

- Cada tienda corre en un proceso. Los sockets TCP, como en los demos de cliente y servidor de clase, conectan a compradores, tiendas y monitor. El monitor solo consulta; una tienda puede vender aunque el monitor esté desconectado.
- Los pares se identifican con `SALUDO`. Intercambian ventas individuales identificadas por origen y número de secuencia, y cada tienda suma el total en su propio registro. El comando `ESTADO` es para el monitor. `PENDIENTES` permite seguir el vaciado de envíos durante la demo.
- El origen registra primero su venta. Los envíos iniciales siguen el orden de conexión de cada origen. El publicador espera la confirmación o el tiempo límite de cada intento. Un par que vuelve entra al final del orden activo. Al recuperar registros pendientes, la tienda los lee en el orden disponible.
- En las tiendas de cadena, dos hilos manejan solicitudes TCP y un tercero publica ventas. Bakery de Lamport protege el registro: cada hilo anuncia que elige, toma un número, desempata por identificador y pone su turno en cero al salir. Las esperas de red quedan fuera de esa sección. La escritura y el `fsync()` se hacen dentro del turno para que el registro y los totales avancen juntos. Una escritura lenta puede retrasar otra actualización local.
- Si un par vuelve, recibe las ventas que se le debían. Si el origen se apaga, las tiendas activas pueden recuperar las copias que otra alcanzó a registrar. La combinación de origen y número evita duplicados. Si la única copia queda en el equipo apagado, se recupera cuando ese equipo vuelva con su archivo.
- Cada tienda deja de vender cuando su copia del total alcanza la meta. Si dos tiendas aceptan compras simultáneas a partir de copias atrasadas, ambas ventas válidas se registran y el total puede superar $100. Los totales pueden diferir mientras haya una desconexión. En WSL el corte de luz se simula con `SIGKILL`. El cierre por meta permite responder consultas y descargar lo pendiente antes de que el script termine los procesos.

## Qué se confirma

La tienda responde `OK REGISTRADA` después de completar la escritura y sincronizar el archivo local. Una confirmación de otra tienda acredita ese mismo paso en esa tienda. Durante el corte, `PENDIENTES 0` se refiere a las tiendas que responden. Las copias almacenadas únicamente en un equipo apagado se recuperan cuando ese equipo vuelve con su registro.
