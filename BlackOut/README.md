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

Si ya lo clonaste en tu directorio personal, entra con `cd ~/Proyecto-SO/BlackOut`. Ejecuta el escenario que quieres probar.

```bash
bash demo.sh cierre
bash demo.sh apagon
```

La demo compila el programa y levanta cuatro tiendas en el mismo equipo. A, B y C forman una cadena. D es independiente. Cada tienda tiene su propio simulador de compradores, que espera entre 0,30 y 1,10 segundos y elige compras de $7.00 a $16.00 al azar. Los compradores funcionan al mismo tiempo. Un monitor consulta las tiendas cada segundo por separado y muestra sus ventas propias y su total en dólares. Los totales pueden diferir mientras viajan las ventas.

- En `cierre`, las tiendas parten de cero con una meta de **$100.00**. Observa cuándo dejan de aceptar compras y cuánto suman las ventas ya registradas.
- En `apagon`, la demo interrumpe el proceso de B con `SIGKILL`. A y C pueden seguir atendiendo compras mientras estén abiertas. El monitor marca a B sin respuesta y muestra el último dato que pudo consultar.

Por defecto la demo genera compras durante 18 segundos, usa cuatro puertos desde el 18080 y provoca el apagón tras el primer segundo. Al salir, termina los procesos y borra los archivos temporales. Puedes cambiar el puerto inicial, la duración en segundos, la meta en dólares y el segundo del apagón.

```bash
BLACKOUT_PUERTO_BASE=18100 BLACKOUT_DURACION=15 BLACKOUT_META=150.00 \
  BLACKOUT_APAGON_EN=2 bash demo.sh apagon
```

Con `BLACKOUT_MOSTRAR_REGISTROS=1` puedes ver al final el registro de ejecución de cada tienda.

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

## Decisiones que conviene poder explicar

- Cada tienda corre en un proceso y se comunica directamente por TCP con las demás. Las tiendas intercambian ventas identificadas por origen y número de secuencia. Cada una suma el total en su propio registro.
- El origen registra su venta primero. Después, el publicador la envía en el orden de conexión de los pares activos. Un par que vuelve a conectarse queda al final del orden activo. Los envíos de distintos orígenes pueden intercalarse.
- Bakery de Lamport coordina los dos hilos que comparten el registro dentro de cada tienda. Cada hilo toma un número y se compara con el otro. Las esperas de red ocurren fuera de la sección que protege el registro.
- Al arrancar, cada tienda carga las ventas de su archivo. Se reintentan entregas pendientes a pares recuperados. Si quedó una copia en otra tienda, esa tienda puede ayudar a recuperarla. Cuando la única copia quedó en un equipo apagado, habrá que esperar a que vuelva. Una venta repetida conserva el mismo identificador y cuenta una sola vez.
- Al alcanzar o superar la meta, la tienda rechaza nuevas compras. En la demo el proceso cerrado sigue activo durante el monitoreo y recibe ventas pendientes. El apagón se prueba interrumpiendo el proceso de B.

Para practicar, predice qué mostrará cada tienda antes de correr la demo. Explica por qué una venta en A aumenta el total de B sin cambiar las ventas propias de B. Luego repite el ejercicio con B desconectada.
