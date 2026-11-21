# BlackOut · referencia de estudio

Esta referencia se preparó con IA para practicar los conceptos del proyecto. La rúbrica del curso excluye el código generado por IA de la entrega.

## Ejecutar la demo

Si usas Windows con Ubuntu en WSL, abre la aplicación **Ubuntu** e instala las herramientas si te hacen falta.

```bash
sudo apt update
sudo apt install -y build-essential git
```

Descarga el repositorio y entra en la carpeta del proyecto.

```bash
cd ~
git clone https://github.com/AraujoHivor/Proyecto-SO.git
cd Proyecto-SO/BlackOut
```

Si ya lo clonaste, actualiza y ejecuta esta sola demostración.

```bash
cd ~/Proyecto-SO/BlackOut
git pull --ff-only
bash demo.sh
```

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

## Cómo funciona

- Cada tienda corre en un proceso. Los sockets TCP, como en los demos de cliente y servidor de clase, conectan a compradores, tiendas y monitor. El monitor solo consulta; una tienda puede vender aunque el monitor esté desconectado.
- Los pares se identifican con `SALUDO`. Intercambian ventas individuales identificadas por origen y número de secuencia, y cada tienda suma el total en su propio registro. El comando `ESTADO` es para el monitor. `PENDIENTES` permite seguir el vaciado de envíos durante la demo.
- El origen registra la venta primero. El publicador envía la venta siguiendo el orden en que respondieron los pares. Espera la confirmación o el tiempo límite de cada intento. Un par que vuelve a responder entra al final del orden activo.
- En las tiendas de cadena, dos hilos manejan solicitudes TCP y un tercero publica ventas. Bakery de Lamport protege el registro: cada hilo anuncia que elige, toma un número, desempata por identificador y pone su turno en cero al salir. Las esperas de red quedan fuera de esa sección.
- Si un par vuelve, recibe las ventas que se le debían. Si el origen se apaga, las tiendas activas pueden recuperar las copias que otra alcanzó a registrar. La combinación de origen y número evita duplicados. Si la única copia queda en el equipo apagado, se recupera cuando ese equipo vuelva con su archivo.
- La tienda deja de aceptar compras al conocer que su total aplicable alcanzó la meta. Las compras ya registradas pueden hacer que se supere ese valor. En WSL el corte de luz se simula con `SIGKILL`. El cierre por meta permite responder consultas y descargar lo pendiente antes de que el script termine los procesos.
