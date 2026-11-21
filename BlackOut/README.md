# BlackOut · referencia de estudio

Esta referencia se preparó con IA para practicar los conceptos del proyecto. La rúbrica del curso excluye el código generado por IA de la entrega.

## Ejecutar la demo

En Linux, abre una terminal en esta carpeta. Necesitas Bash y GCC.

```bash
bash demo.sh cierre
bash demo.sh apagon
```

La demo compila el programa y abre cuatro procesos que consulta por `127.0.0.1`. A, B y C forman una cadena. D lleva únicamente sus propias ventas. En cada ronda se lanzan tres compradores con montos aleatorios y después se consulta el estado de las tiendas. Los totales pueden diferir mientras quedan ventas en tránsito. Al final hay otra consulta tras una pausa breve. El script termina los procesos y elimina sus archivos temporales al salir.

- En `cierre`, la cadena y la tienda independiente tienen metas bajas. Observa cómo cambia el estado y qué responde una tienda a una compra posterior al cierre.
- En `apagon`, la tienda B se interrumpe al empezar la tercera ronda. Observa qué pueden seguir haciendo A y C y qué dato muestra el monitor cuando B no responde.

La demo empieza en el puerto `18080` y usa cuatro puertos consecutivos. Puedes elegir otro puerto, cambiar el número de rondas y ajustar la pausa en segundos.

```bash
BLACKOUT_PUERTO_BASE=18100 BLACKOUT_RONDAS=12 BLACKOUT_PAUSA=5 bash demo.sh apagon
```

## Compilar y consultar a mano

```bash
gcc -std=c11 -Wall -Wextra -Werror -pthread blackout.c -o blackout
```

En una terminal, inicia A con B como única vecina, una meta de 1000 centavos y el archivo `ventas-A.log`.

```bash
./blackout tienda A 18080 1000 ventas-A.log 127.0.0.1:18081
```

En otra terminal, inicia B con la misma meta y la dirección de A.

```bash
./blackout tienda B 18081 1000 ventas-B.log 127.0.0.1:18080
```

En una tercera terminal, registra una compra de 350 centavos en A y consulta ambas tiendas.

```bash
./blackout cliente 127.0.0.1 18080 COMPRA 350
sleep 0.1
./blackout cliente 127.0.0.1 18080 ESTADO
./blackout cliente 127.0.0.1 18081 ESTADO
```

Si las dos arrancaron desde cero y recibieron la venta, A reporta `ESTADO A ABIERTA 350 350` y B reporta `ESTADO B ABIERTA 0 350`. Los dos últimos campos son las ventas propias y el total de la cadena, en centavos. Para iniciar una tienda independiente, omite las direcciones de pares. Para usar equipos distintos, indica en cada lista las direcciones alcanzables desde ese equipo.

## Decisiones que conviene poder explicar

- Cada tienda es un proceso y se comunica directamente con sus pares por sockets TCP. Cada venta lleva un origen, un número de secuencia y el monto. El comprador y el monitor se conectan como clientes de la tienda.
- La tienda de origen registra su venta primero y responde al comprador con `OK REGISTRADA` cuando la guarda localmente. Un hilo difunde la venta a los pares en el orden de la lista y espera la respuesta de cada par antes de intentar el siguiente. Ventas de otros orígenes pueden intercalarse.
- Un bloqueo por turnos protege la actualización local de los montos y el estado entre hilos. El turno cubre el acceso al estado de una tienda. Las esperas de red quedan fuera del turno.
- Si un par falla o supera el tiempo de espera, la tienda sigue con el siguiente. Al arrancar recupera las ventas de su propio archivo. Las ventas que quedaron sin confirmar durante un corte carecen de reenvío automático, así que una tienda aislada puede conservar un total distinto.
- Al alcanzar o sobrepasar la meta, la tienda rechaza nuevas compras. El proceso permanece activo en la demo para atender consultas y recibir ventas pendientes de otras tiendas. Un corte real se representa deteniendo el proceso. Si la tienda ya estaba desconectada, el monitor identifica su último dato como histórico.

Para practicar, predice qué mostrará cada tienda antes de correr la demo. Explica por qué una venta en A aumenta el total de B sin cambiar las ventas propias de B. Luego repite el ejercicio con B desconectada.
