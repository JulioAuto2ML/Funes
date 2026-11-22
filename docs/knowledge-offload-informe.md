# Facts Outside the Weights — informe legible para julio

**Fecha:** 2026-11-22 · **Qué es esto:** una versión del paper pensada para *leerse*, no para enviarse a una revista. Explica qué se hizo, qué dio, qué tan firme es cada resultado, qué se podría rehacer y qué se puede reportar como está. El borrador técnico (denso, con placeholders) sigue siendo `docs/knowledge-offload-paper-draft.md`; el plan original es `docs/knowledge-offload-plan.md`; el cuaderno de laboratorio es `benchmarks/knowledge-offload/NOTEBOOK.md`.

**Cómo se hizo este informe, y qué no verifiqué.** Leí completos el plan (723 líneas) y el borrador (463 líneas) y las últimas entradas del NOTEBOOK. Los números de abajo están copiados de las tablas commiteadas y del borrador; **no los recalculé uno por uno**. Medí yo dos cosas hoy, directamente desde los resultados commiteados, porque el borrador las dejaba como pendientes o desactualizadas: el `retrieval_hit` del store (0,993 en los cinco tamaños) y la existencia de las filas `funes` (ver §6). Todo lo que no pude comprobar está marcado «no verificado» o «no medido».

---

## 1. De qué trata, en tres párrafos

Un modelo de lenguaje guarda en sus pesos dos cosas mezcladas: **cómo hacer cosas** (seguir una instrucción, llamar una herramienta, sumar) y **qué es verdad** (quién dirigió esta película, en qué país queda este pueblo). Lo segundo es caro de guardar en pesos, queda viejo al día siguiente del entrenamiento, no se puede inspeccionar ni editar, y es justo lo que los modelos chicos hacen peor.

La pregunta que originó todo es de ingeniería: **si los hechos viven en otro lado (una base de datos que el modelo consulta), ¿cuánto modelo hace falta para el resto del trabajo? ¿Un modelo más chico puede caber en menos VRAM?** En yoda eso es concreto: el Qwen 27B ocupa 13,7 de los 16 GB de la placa; cada GB liberado es contexto, o un segundo modelo cargado al lado, o generación de imágenes sin tener que apagar nada.

El paper intenta contestar la parte que se puede contestar **solo con inferencia** (sin reentrenar nada), midiendo dónde puede vivir un hecho —en los pesos, en una base local, en la web— y qué cuesta cada opción. Después, por decisión tuya (2026-11-20, ruta C), se agregó un experimento de entrenamiento chico: **un LoRA que le enseña al modelo a consultar la base por sí mismo** (E5). Lo que el trabajo *no* puede decir, y el plan lo avisa desde el principio: nada de «liberar parámetros». Un LoRA cambia comportamiento, no capacidad. Para liberar capacidad de verdad hace falta preentrenar con los hechos enmascarados (LMLM, Co-LMLM), que son semanas de GPU.

---

## 2. Cómo está armado el experimento (lo mínimo para entender los resultados)

**Las preguntas.** PopQA: preguntas de un solo dato («¿Quién dirigió X?», «¿En qué país queda Y?») generadas a partir de triples de Wikidata, con la popularidad de cada entidad medida. Es útil porque lo difícil para un modelo chico es la **cola larga** (entidades poco famosas). Se usan 1.000 preguntas *no ambiguas* (una sola respuesta posible), repartidas por nivel de popularidad (cinco niveles de popularidad tienen ~195 preguntas cada uno; los extremos tienen 16, 8 y 2 y no se leen).

**Los modelos.** Una sola familia, Qwen3.5, en cinco tamaños (0,8 / 2 / 4 / 9 / 27B), cuantizados a IQ4_XS (4 bits), más una curva de control en Q8_0 (8 bits) hasta 9B. Cada archivo tiene su sha256 en `models.lock` y el runner se niega a correr un archivo cuyo hash no esté ahí. Eso salió de un error real (ver §8).

**Las condiciones**, en palabras llanas:

| condición | dónde está el hecho | qué hace el modelo |
|---|---|---|
| `closed` | en los pesos | contesta de memoria |
| `oracle` | en el prompt, servido en bandeja | le damos el triple exacto: es el techo |
| `store` | en una base local; el *harness* busca (BM25) y lo pega en el prompt | lee y contesta |
| `tool` | en la base local; el *modelo* decide si consulta | tiene una herramienta `lookup` y elige usarla o no |
| `tool+contract` | igual | el harness no acepta una respuesta hasta que haya consultado (la «obligación») |
| `web` | en internet | tiene `web_search` |
| `web+writeback` | internet + caché local | lo buscado se guarda y se repite la pasada |
| `funes` | en la memoria del agente real | la pregunta pasa por el bucle completo de Funes |

**La métrica principal** es `contains`: la respuesta normalizada contiene al objeto correcto. Se eligió después de ver (en E0) que en preguntas ambiguas esta métrica premia listar candidatos, y por eso el análisis serio se hizo solo sobre las no ambiguas.

**La estadística.** Todo pareado (las mismas preguntas en cada condición), intervalos del 95% por bootstrap sobre las preguntas (10.000 remuestreos), una sola corrida por celda porque con decodificación codiciosa (temperatura 0) el resultado es determinista (se comprobó: 0 de 1.000 respuestas distintas entre repeticiones en E0, y 0 de 100 en el piloto de E1). Por eso **los intervalos solo cubren la variación entre preguntas, no la variación entre corridas, servidores o semillas de entrenamiento.** Hay una familia de hipótesis confirmatorias corregidas con Holm (m = 6: H1, H2, H3, H4, H5, H7); las que todavía no tienen datos entran con p = 1, o sea que todos los p ajustados son cotas conservadoras.

---

## 3. Qué se hizo y qué se encontró

Cada bloque dice: qué se hizo, qué dio, cómo leerlo, y **qué tan firme es**. Al final de cada uno va mi recomendación: *reportar tal cual*, *rehacer* o *completar*.

### 3.1 Darle los hechos aplana la curva de tamaño (H1) — el resultado principal

**Qué se hizo.** Cinco tamaños × tres condiciones (`closed`, `oracle`, `store`) sobre las 1.000 preguntas.

**Qué dio.**

| acierto (`contains`) | 0,8B | 2B | 4B | 9B | 27B |
|---|---|---|---|---|---|
| `closed` (de memoria) | 0,213 | 0,243 | 0,320 | 0,350 | 0,431 |
| `store` (con la base) | 0,921 | 0,971 | 0,984 | 0,980 | 0,991 |
| `oracle` (techo) | 0,972 | 0,995 | 0,997 | 0,987 | 0,997 |

- De memoria, cada vez que el modelo se multiplica por 10 gana 0,146 de acierto. Con la base gana 0,040. La relación entre las dos pendientes es **0,27** (IC 0,21–0,35); en Q8_0, 0,21. El umbral que fijamos antes de correr era < 0,5. Se cumple en las dos cuantizaciones.
- La comparación más gráfica: **el 0,8B con base (0,921) le gana al 27B de memoria (0,431) por 0,490** (IC 0,457–0,523). 510 preguntas solo las acertó el chico con base; solo 20 las acertó el grande de memoria.
- Dicho en simple: **gran parte de lo que compraban los parámetros era memoria de hechos.** Con los hechos afuera, un modelo de 0,8B casi alcanza a uno de 27B en esta tarea.

**Cómo leerlo, con los matices que le corresponden.**
1. **`store` está en el techo por construcción.** Las preguntas de PopQA se generan de los mismos triples que están en la base. Encontrar el dato es trivial (confirmé hoy: `retrieval_hit` = **0,993** en los cinco tamaños, calculado de los JSON commiteados; el criterio de «kill» del plan era < 0,9, no se cumple). Esto significa que H1 habla de *recibir* hechos, no de *encontrarlos en una base desordenada*. La base desordenada es la situación real de Funes y está pendiente (E4 y las filas `funes`).
2. Parte de la pendiente de `store` es de lectura, no de conocimiento: el 0,8B pierde 5,1 puntos entre recibir el triple exacto y recibirlo de la búsqueda (0,972 → 0,921); desde el 9B para arriba esa diferencia es ruido. El plan decía que un hueco > 10 puntos se reportaría como límite de capacidad de lectura; el observado es 5,1.
3. Cinco puntos de una sola familia: es un ajuste a cinco checkpoints, no una ley de tamaño.

**Firmeza: alta** para lo que afirma (H1 se sostiene; p ajustado por Holm 0,0006). **Recomendación: reportar tal cual**, con los dos matices en el texto. Un límite no menor: H1 en Q8_0 no se puede probar completo porque no hay 27B Q8_0 (pesa 28,6 GB y no entra en la placa).

### 3.2 Cuando el modelo decide solo si consulta (H8): no depende del tamaño

**Qué se hizo.** La condición `tool` con un prompt neutro («usá las herramientas cuando ayuden»), los cinco tamaños y las dos cuantizaciones, 36 celdas.

**Qué dio.** Porcentaje de preguntas en que el modelo llamó a `lookup` (IQ4_XS): 0,8B **0,000**, 2B **0,000**, 4B **0,875**, 9B **0,001**, 27B **0,887**. Es decir: el 9B casi nunca consulta aunque el 4B sí. Y cuando no consulta, contesta de memoria y su acierto es igual al de `closed`.

**Cómo leerlo.** La hipótesis H8 («consultar crece con el tamaño») **no se sostiene**. El comportamiento es una propiedad del checkpoint y del texto del prompt, no del tamaño. Tampoco hay que sacar una «pendiente por década» de esto: describiría el ajuste, no una ley.

**El experimento de las variantes.** Para ver si el salto del 9B era del checkpoint o del texto, se fijaron antes de correr tres reescrituras del prompt (`tool_first`, `tool_free`, `tool_describe`) y se corrieron en 9B y 4B, en las dos cuantizaciones (12 celdas). Resultado: **sacar la frase que pide la respuesta corta (`tool_free`) lleva al 9B de 0,001 a 0,732 de consultas** (0,763 en Q8_0); reordenar las frases o describir la herramienta no lo mueve (0,000). Un modelo que «no sabe usar la herramienta» resultó ser un modelo cuya frase de formato le gana a la de usar herramientas. No sabemos por qué (se probó una sola eliminación, no otras), y el paper no afirma un mecanismo.

**Atención al leer `tool_free`:** sin la frase de formato, las respuestas son oraciones (50,6 tokens contra 3,9), así que su acierto (0,933) no es comparable con el de las otras celdas: `contains` premia respuestas largas que mencionen el dato. Lo que sí sostiene esa celda es la tasa de consulta.

**Firmeza: media-alta** para «no hay una ley de tamaño y el prompt manda»; es una sola familia, una corrida por celda, y las variantes son descriptivas (sin p-valores, fuera de Holm). **Recomendación: reportar tal cual, como rango sobre cuatro redacciones del prompt y sin enunciar mecanismo.** Las etapas C (27B) y D (2B) de las variantes no se corrieron porque las condiciones que las disparaban no se dieron: se podrían correr si querés cerrar el hueco (costo no medido; el 27B se extrapoló en 2,3–2,6 h).

### 3.3 Obligar la consulta (H5): arregla a los chicos, y no podemos probar que sea gratis

**Qué dio.**
- Con la obligación el 9B pasa de 0,347 a **0,991** de acierto (+0,644, 644 preguntas a favor y 0 en contra). Al 0,8B no le sirve (nunca llama a la herramienta ni insistiéndole). Al 2B IQ4_XS le sirve para el 21% de las preguntas; al 2B Q8_0, 96%.
- **H5a (la obligación no cuesta exactitud donde el modelo ya consulta): NO demostrada.** En 4B y 27B la obligación suma ~1,7 puntos (en la misma dirección en ambos), pero la prueba de equivalencia ±2 puntos no llega (p = 0,135 agrupado; 0,541 tras Holm). Con 1.000 preguntas y estas exactitudes simplemente no hay potencia para probar equivalencia a ese margen.
- **H5b (alucinación cuando el hecho no está en la base):** la obligación baja las respuestas incorrectas donde el modelo no consultaba (2B −0,27, 9B −0,325); donde ya consultaba, no cambia. Es el efecto de «hacer que consulte», no una propiedad mágica de la obligación.

**Un aviso metodológico del propio paper:** los p-valores agrupados de H5b tratan las mismas 200 preguntas en cinco tamaños como independientes y son optimistas. La entrada de Holm para H5 es el máximo de H5a y H5b, y por eso H5 **no sobrevive la corrección** (0,676).

**Firmeza:** el efecto (la obligación rescata a quien no consulta) es sólido y grande. La afirmación «no cuesta nada» es la que no se pudo sostener. **Recomendación: reportar tal cual, incluida la negativa.** Rehacer H5a con más potencia exigiría muchas más preguntas (otra muestra); no se calculó cuántas, no medido.

### 3.4 La web como base de hechos: peor que la base local

**Qué dio.** `web` (búsqueda de Parallel): 0,8B 0,436, 9B 0,642 contra 0,921 y 0,980 del `store`. La pérdida es −0,485 y −0,338. Cuando el dato estaba en los fragmentos que vio el modelo, acertó ~76% de las veces; cuando no estaba, ~0–5%. **El límite está en lo que la búsqueda devuelve, no en cómo lee el modelo.**

**Qué no se pudo:** la celda del 27B es **inválida** — se acabó el crédito de Parallel (USD 20) a mitad de la corrida: 1.661 de 2.086 búsquedas devolvieron error 402, que el harness convirtió en texto de herramienta sin registrar error, y un informe llegó a presentar el menor acierto del 27B como un hallazgo hasta que una auditoría lo desarmó (§8). Se reporta el 27B solo sobre las 424 preguntas limpias (27B 0,748 contra 9B 0,623 en esas mismas), etiquetado como parcial. No hay celdas web en 2B ni 4B. Tavily está reservado para producción.

**Firmeza:** media. Dos tamaños completos, uno parcial, una sola API de búsqueda (su calidad es un confusor de todo número web). **Recomendación: reportar como está, con la etiqueta «parcial»; completar solo si aprobás un presupuesto escrito** (el 27B completo necesita ≈ 2.100 búsquedas; el costo por búsqueda no está medido, hay que leerlo de la facturación de Parallel).

### 3.5 Write-back (H6): falla

Guardar lo buscado para no repetirlo (la caché «demanda») **no recuperó el acierto**: la pasada 1 quedó 0,219 por debajo de `web` simple y la 2 sigue 0,180 por debajo. El criterio «llamadas de pasada 2 ≤ 10% de las de pasada 1» tampoco se cumple (45%). Además el brazo confunde la caché con una herramienta extra (`recall`) y el control que lo separaría (`web+recall-empty`) nunca se corrió. **Recomendación:** decisión tuya — o se saca del paper, o se corre el control (necesita presupuesto de búsqueda). Reportarlo como negativo exploratorio sin el control es defendible si se dice que el diseño tiene ese confuso.

### 3.6 Razonamiento sin hechos (H2): el control negativo sale bien

**Qué se hizo.** 500 problemas de GSM8K (aritmética con enunciado, no necesita hechos), cinco tamaños, `closed` y `store` (con cinco hechos irrelevantes pegados).

**Qué dio.** El acierto sube con el tamaño (0,8B 0,436; 2B 0,726; 4B 0,910; 9B 0,934; 27B 0,952) y la base **no aplana nada**: relación de pendientes 1,04 (IC 0,959–1,124). H2 confirmada por la regla fijada antes de correr.

**Cómo leerlo.** Esto *es* un control para H1: demuestra que el aplanamiento de PopQA no es algo que el harness produzca con cualquier tarea. No es un descubrimiento sobre razonamiento (el store acá es irrelevante por construcción). Detalle honesto: los hechos irrelevantes cuestan entre 0,8 y 3,6 puntos de acierto en los nueve pares tamaño×cuantización, siempre en contra; las ganancias de §3.1 son netas de ese costo. Y el 27B (0,952) pasa el techo de 0,95 del diseño; sin él la conclusión no cambia.

**Firmeza: alta.** **Recomendación: reportar tal cual.**

### 3.7 E5 — ¿un LoRA puede enseñar a consultar? (ruta C)

Es el último experimento y el más reciente, así que lo detallo más.

**Qué se hizo.** Se entrenó un LoRA sobre Qwen3.5-2B (principal) y 0,8B (secundario), con tres brazos por modelo:
- **base** — el modelo sin tocar;
- **L1** — entrenado con trayectorias «pregunta → llamada a `lookup` → resultado → respuesta»;
- **L0** — el control: *mismas preguntas, mismo bloque de herramientas, mismas respuestas finales*, pero contesta directo, sin llamada. Sin L0, cualquier mejora de L1 podría ser el formato corto o las plantillas de PopQA, no la consulta.

Los datos no los escribe ningún modelo: salen de las 14.267 triples de PopQA (1.000 ejemplos de consulta por modelo) más 500 «anclas» (respuestas del propio modelo base a preguntas de entrenamiento de GSM8K, para que no se desvíe donde no hay que consultar). Ningún id de evaluación ni ninguna entidad de evaluación está en el entrenamiento (se afirma en código). Se **retuvieron tres relaciones** nunca vistas (director, father, screenwriter: 244 preguntas de evaluación). Se eligió entre tres configuraciones de hiperparámetros fijadas antes (en dev empataron las tres en 1,000 de consulta; el desempate pre-fijado eligió la B). Costo medido: ~2,4 h de GPU en total para siete entrenamientos; la ventana de evaluación duró 2 h 44 min. Antes de leer ningún resultado pasaron los controles de plantilla y máscara, y por cada adaptador la prueba «con escala 0 reproduce al base, con escala 1 cambia».

**Qué dio (familia pre-registrada de tres tests, Holm m = 3):**

| prueba | regla escrita antes | resultado | veredicto |
|---|---|---|---|
| **H9a** ¿consulta por sí mismo? | L1 2B ≥ 0,80 de consultas, en las 1.000 **y** en las relaciones retenidas | **0,992** en las 1.000; **1,000** en las 244 retenidas | **se cumple** (p aj. 0,0003) |
| **H9b** ¿es la consulta y no el formato lo que ayuda? | acierto(L1) − acierto(L0) con IC inferior > 0 y diferencia ≥ +0,10 | **+0,705** [+0,676; +0,733]. L0 no consulta nunca (0,000) y acierta 0,285; L1 acierta 0,990 | **se cumple** (p aj. 0,0003) |
| **H9c** ¿las habilidades siguen intactas? | GSM8K `closed`, L1 − base, IC 95% dentro de ±0,05 | **−0,034** [−0,074; +0,006] (0,726 → 0,692) | **no se cumple** (p aj. 0,24) |

**Cómo leer cada uno.**
- **H9a** es el resultado limpio: bajo el prompt neutro el modelo base consulta 0 de 1.000 veces; con el LoRA, 992. Donde las obligaciones de §3.3 dejaban al 2B IQ4_XS en 21% y al 0,8B en 0%, el LoRA lo lleva a ~99% (0,8B: 0,995). Un detalle: el intervalo «[1,000; 1,000]» de las retenidas es un artefacto del bootstrap (244 de 244); la cota inferior exacta de 95% es ≈ 0,985, calculada a mano por Opus.
- **H9b** vale pero **no hay que sobreinterpretarlo**: casi está garantizado una vez que L0 no consulta nunca, porque L1 recibe el dato de la base. Prueba que la ganancia no es de formato; **no** prueba que el modelo «sepa» más. El ≈ 0,99 es la base devolviendo el hecho (el 9B con la obligación de §3.3 también llegó a 0,991).
- **H9c no se cumple, y no es una pérdida demostrada.** El intervalo incluye al 0, pero su límite inferior (−0,074) cae fuera del margen, así que la equivalencia no se muestra. **Lo que sí quedó medido y descarta una explicación fácil:** repetí el modelo base sobre los mismos 500 problemas y las 500 respuestas salieron *idénticas* (discordancia 0,000). O sea que el 20,2% de problemas en que L1 y base discrepan **es efecto del adaptador, no ruido**. Un solo servidor, una sola repetición.
- **El control se mueve igual (exploratorio, calculado después de ver la tabla, no pre-registrado):** en 0,8B, L1 − base = −0,088 y L0 − base = −0,086, o sea que el descenso en GSM8K es **de la receta de entrenamiento, no de la consulta** (L1 − L0 = −0,002). En 2B, L1 − L0 = −0,018 [−0,048; +0,012], indistinguible de cero. Por qué baja GSM8K con la receta: no se sabe y habría que hacer una ablación que no se corrió.

**Otros resultados (solo reportados, sin p-valores):**
- Cerrado (sin herramienta) el adaptador no hizo que el modelo dejara de contestar de memoria: 2B 0,243 → 0,238; 0,8B 0,213 → 0,182.
- Preguntas reformuladas con plantillas escritas a mano antes de entrenar (set P, 200 preguntas, dos plantillas por relación): 2B L1 0,965 contra L0 0,260.
- Otras redacciones del prompt (`tool_free`, `tool_describe`), nunca vistas en el entrenamiento: 2B 0,983 y 0,993.
- Mismo adaptador sobre la base Q8_0 (otra cuantización): acierto 0,995, consulta 1,000. Ojo: la prueba de «escala 0 = base» se hizo sobre la base propia del adaptador, no sobre Q8_0.
- **Qué no aprendió, y no se le enseñó a propósito:** cuándo *no* consultar. En el prompt que permite abstenerse, el 2B L1 consulta 1,000 y alucina en 0,540 de las 200 preguntas con el dato ausente (se abstiene 0,265). Enseñar abstención es otro experimento.

**Limitaciones de E5 (todas reales):** una semilla y una corrida por brazo (no hay varianza de entrenamiento); la evaluación está en distribución respecto de las plantillas y la popularidad del entrenamiento; director y screenwriter están cerca de relaciones que sí se entrenaron (producer, author, composer), así que **la relación retenida limpia es father** (50 preguntas, las 50 consultadas); entrenar en bf16 y servir en IQ4_XS no se comparó numéricamente; las definiciones de p-valor de `stats_e5.py` se escribieron *después* de tener los datos y *antes* de leer ningún número (Opus las revisó, las aceptó, y reprodujo la tabla byte a byte); el set del 0,8B y el cambio de reglas `transfer_bases` fueron decisiones tuyas posteriores al diseño.

**Firmeza:** H9a y H9b son sólidos *dentro de lo que miden*; H9c es un negativo con una explicación parcial (efecto de la receta, causa desconocida). **Recomendación: reportar tal cual**, con H9c como «no se cumple, la causa no está probada, el control se mueve igual». Si se quiere cerrar H9c, hay opciones en §7.

### 3.8 Las filas `funes`: corridas y verificadas, pero sin analizar ni en el paper

**Hallazgo del cruce.** El borrador dice en varios lugares que las filas `funes` «no existen todavía». **Sí existen**: cinco celdas, una por tamaño, corridas el 2026-11-18 en yoda y verificadas el 2026-11-19 (1.000 respuestas distintas cada una; 5 archivos `popqa-stratified1u-n1000-qwen3.5-*-funes-pool14267-r0.json`). Acierto `contains`: **0,8B 0,636, 2B 0,715, 4B 0,967, 9B 0,931, 27B 0,985**, una repetición (temperatura 0,2), con el pool completo de 14.267 triples cargado en una cuenta descartable. Comparado con el `store` (0,921 / 0,971 / 0,984 / 0,980 / 0,991), `funes` queda siempre por debajo: la diferencia pareada (`stats_funes.py`, `results/FUNES_TABLE.md`, bootstrap 10.000) es −0,285 [−0,318; −0,253] en 0,8B, −0,256 [−0,286; −0,227] en 2B, −0,017 [−0,030; −0,004] en 4B, −0,049 [−0,067; −0,032] en 9B y −0,006 [−0,015; +0,003] en 27B (solo en el 27B el intervalo incluye al 0). La tasa de «taint» (el agente leyó contenido externo) es 0,181 en 0,8B, 0,078 en 2B y ≤ 0,007 de 4B para arriba: es el número del costo de seguridad que el paper prometía y no tenía. El 9B por debajo del 4B es una repetición de una línea: no se lee como hallazgo.

Un detalle de honestidad registrado en el NOTEBOOK: 17 de 5.000 respuestas fallaron con «el pedido supera el contexto» (0,8B 4, 2B 11, 9B 1, 27B 1); cuentan como incorrectas y el 2B pierde hasta 1,1 puntos por eso. Es comportamiento real del sistema; no se repitió. Tiempos medidos de las corridas: 9B 72 min, 4B 57, 2B 39, 0,8B 40, 27B 233.

**Estado (2026-11-22):** el script de estadística ya existe y los datos pasan sus verificaciones (`stats_funes.py`, 277 tests en verde); falta (b) escribirlo en el paper y (c) corregir el borrador (§0.3 punto 4, §3.3, §5.5, §6, §7.8 y el resumen dicen que no existen). Son tareas **sin GPU**. Esto cubre una parte importante de la afirmación «dentro de un agente desplegado», que hoy el paper dice que no puede hacer. **Recomendación: completar** (es la tarea de mayor valor por costo de todo el informe).

---

## 4. Cruce del plan contra lo hecho

El plan prometía ocho experimentos (E0–E7, con E1b) y diez hipótesis. Estado real:

| ítem del plan | qué prometía | estado | comentario |
|---|---|---|---|
| **E0** harness | 8 condiciones, PopQA 1k, 3 repeticiones planeadas | **hecho**, con un error | corrió sobre un modelo mal identificado (ver §8); sus números se reportan solo como E0, nunca en la curva de tamaño |
| **E1** curva de tamaño | PopQA + EntityQuestions + GSM8K; cinco tamaños; tres repeticiones | **hecho en lo esencial**, con cambios | **EntityQuestions no se corrió** (no aparece en el código, el NOTEBOOK ni el borrador); repeticiones: 1 en vez de 3, tras comprobar determinismo; gráficos: solo tablas (no verifiqué si hay figuras fuera de `results/`: no hay PNG/SVG/PDF allí) |
| E1 `funes` por tamaño | una fila por tamaño | **corrido, sin analizar** | ver §3.8 |
| E1 `web` | en 0,8B, 9B y 27B | **parcial** | 27B inválido, se reporta sobre 424 preguntas; 2B y 4B no |
| E1 `web+writeback` | solo 9B | **hecho; falla** | falta el control `web+recall-empty` |
| E1 brazo `ablated` (H5b) | 200 preguntas con el triple excluido | **hecho** | regex de abstención verificada contra 50 etiquetas tuyas: 49/50 |
| E1 GSM8K | H2 | **hecho** (23/23 celdas) | |
| E1b modelos preentrenados (LMLM/Co-LMLM contra gemelos Standard) | reproducir números, capacidad liberada, ediciones, Funes como backend | **solo prerrequisitos medidos** | corre el 382M en la placa con torch más nuevo; no se evaluó ninguna tarea. Reproducir el preentrenamiento: **estimado** 32–45 días en esta placa |
| **E2** conflictos y ediciones (H3) | override paramétrico por tamaño; supersede vs contradict en Funes | **no corrido** | |
| **E3** cuándo consultar | tres tipos de preguntas; abstención; base ruidosa o incorrecta | **no corrido** | solo hay lo de §3.3 (brazo `ablated`) |
| **E4** forma de la base (vector+FTS vs triples tipados) | extender `memory_eval` | **no corrido** | |
| **E5** LoRA de consulta | LoRA 2–4B; comparar en E1/E2/E3 + GSM8K + BFCL + seguimiento de instrucciones | **hecho en parte** | solo condiciones E1 y GSM8K; **BFCL, seguimiento de instrucciones, E2 y E3 no** |
| **E6** dónde vive el conocimiento (ablación de capas) | | **no corrido** | opcional en el plan |
| **E7** ¿Funes entra en un modelo más chico? | batería de habilidades de Funes; piso de habilidad | **no corrido; la batería no existe** | es la pregunta motivadora del proyecto |

| hipótesis | estado |
|---|---|
| **H1** los hechos aplanan la curva en PopQA | **se sostiene** (0,27; Holm 0,0006) |
| **H2** no la aplanan en GSM8K | **se sostiene** (1,04) |
| **H3** modelos más grandes sobrescriben más los hechos editados | **sin datos** (E2) |
| **H4** enmascarar hechos mejora lo que queda | **sin datos** (E1b) |
| **H5** la obligación no cuesta / baja la alucinación | **a: no demostrada; b: sí donde no consultaba**; no sobrevive Holm |
| **H6** la caché web | **falla** (exploratoria) |
| **H7** un modelo ≤ 8 GB con la base queda a 5 puntos del 27B en habilidades Funes | **sin datos** (E7) |
| **H8** consultar crece con el tamaño | **no se sostiene** (exploratoria) |
| **H9a/b/c** (E5, familia propia) | **a y b se cumplen; c no** |

Otras desviaciones del plan, todas registradas en el NOTEBOOK: H5 y H8 se reformularon tras E0/E1 (D12–D14); H6 pasó a exploratoria; Holm se fijó en m = 6 aunque haya hipótesis sin correr; el juez LLM del plan **no se usó** (solo regex + 50 etiquetas tuyas, no las 100 que el plan mencionaba); el plan decía que el «checkpoint» para el preprint era E1–E4 + E1b, y **ese checkpoint nunca se alcanzó** porque se saltó a E5 (decisión tuya, 2026-11-20).

**Cambios a Funes (tabla «What it changes in Funes» del plan):** ninguno. Las condiciones de disparo eran: `require_tools` para agentes factuales (si E3 muestra que forzar gana), tabla tipada de hechos (si E4 gana), ruta `lookup` (si E1b paso 4 funciona), modelo más chico por defecto (si E7 lo encuentra). Ninguna de las cuatro evidencias existe todavía. Lo medido en §3.3 es **indicio** a favor de `require_tools` para modelos chicos, pero E3 —que era la evidencia definida en el plan— no se corrió.

---

## 5. Qué dice el conjunto, y qué no

**Lo que el trabajo sostiene hoy**, con la evidencia medida:
1. Dándole los hechos al modelo, la curva de exactitud contra tamaño se aplana mucho en una tarea de memoria (0,27) y no se aplana en una de razonamiento (1,04). El 0,8B con base supera ampliamente al 27B de memoria.
2. Que un modelo consulte solo por una herramienta depende del checkpoint y de la redacción del prompt; no del tamaño. Una obligación del harness lo resuelve para los modelos que no consultaban, pero **no pudimos demostrar que esa obligación sea gratis** donde el modelo ya consulta.
3. La web como fuente es mucho peor que la base local con este buscador, y limitada por lo que devuelve la búsqueda.
4. Un LoRA chico (999 ejemplos, 2 épocas, ~25 min en esta placa) hace que un modelo de 2B o 0,8B consulte en ~99% de las preguntas, incluidas relaciones no vistas, y la ganancia se debe a la consulta y no al formato. Cuesta en GSM8K ~3 puntos (2B, no demostrado) y ~9 (0,8B), y en 0,8B ese costo es de la receta, no de la consulta (el control cae igual).

**Lo que no sostiene** (el borrador lo dice en §1 y §7; lo repito porque es lo que más se va a querer concluir de más):
- No hay evidencia de **capacidad liberada**: nada de lo medido muestra que un modelo con base necesite menos parámetros para *hacer cosas*. El único resultado cercano (E5) es de comportamiento, y H4 sigue sin probarse.
- No responde la **pregunta del título** («¿cuánto modelo necesita un agente local?»): esa es H7/E7 y la batería de habilidades no existe. Lo que sí dice, hoy, es «cuánto modelo hace falta *para saber hechos*» (poco, si se los das).
- **Una familia, una placa, una corrida por celda**, preguntas generadas desde los mismos triples que la base (techo por construcción).
- Sobre el agente real (Funes) hay datos de las filas `funes` pero no están analizados.

---

## 6. Dónde el borrador técnico está desactualizado (lo encontré al cruzar)

1. **Filas `funes`:** dice que no existen (§0.3 ítem 4, §3.3, §3.5, §5.5, §6, §7.8, resumen y contribución 4); existen y están verificadas desde 2026-11-19 (§3.8 de este informe).
2. **`retrieval_hit`:** el borrador deja un `[TBD]` en §5.2 porque «el número está en los archivos de celda pero no se copió»; lo calculé hoy: 0,993 en los cinco tamaños (de los JSON commiteados; no corrí el script de estadísticas).
3. **El ítem 11 de §0.3** (intervalo de H1 con 20.000 remuestreos y bootstrap que considere dependencia entre tamaños) sigue sin hacerse; no verifiqué si otra sesión lo hizo.
4. El conteo de placeholders del final (`31 [TBD`, `5 [OPEN`, `10 [UNVERIFIED`) es de la versión de Opus de ayer; si se actualiza el borrador hay que recontar con `grep`.
5. La citación de **AtlasKV y KBLaM** (arXiv 2410.10450) quedó **sin verificar**; y 10 de los papers citados solo se conocen por título (el borrador pide una lectura completa de cada uno antes de enviarlo). La frase de novedad del plan («nadie tiene la comparación de tres vías dentro de un agente») necesita revisarse contra la literatura: nadie lo hizo todavía en esta sesión.

---

## 7. Qué se puede reportar como está, qué conviene rehacer, qué falta

Las categorías: **A** = reportar tal cual (ya está sólido); **B** = completar sin GPU (solo trabajo de escritorio); **C** = rehacer o ampliar con GPU o presupuesto; **D** = decisión tuya de alcance.

### A. Reportar tal cual
- H1 con todas sus cautelas (§3.1) y H2 como control negativo (§3.6).
- H8: rango según redacción, sin mecanismo, sin ley de tamaño (§3.2).
- H5: el rescate de la obligación, la alucinación (H5b) y la negativa honesta de H5a (§3.3).
- Web, 27B como parcial, y el error de crédito agotado contado tal cual (§3.4).
- H6 como negativo exploratorio con la advertencia del control faltante (§3.5).
- E5 H9a y H9b, H9c como «no se cumple», el efecto de la receta que también aparece en L0, el 0,000 de base contra base, la abstención no enseñada (§3.7).
- La sección de «cómo se organizó el trabajo» (4.7 del borrador): errores, límites y protocolo. Es parte del aporte metodológico.

### B. Completar sin GPU (barato, de mayor valor)
1. **Analizar las filas `funes`** (script con IC pareados contra `store`) y reescribir las secciones del borrador que las dan por inexistentes (§3.8).
2. **Poner el `retrieval_hit`** (0,993) en §5.2.
3. **Intervalo de H1 con 20.000 remuestreos** y un remuestreo que respete la dependencia entre tamaños (ítem 11 del borrador).
4. **Verificar los papers citados** con lectura completa y reescribir §2; verificar AtlasKV/KBLaM; revisar la frase de novedad.
5. **Respaldar los payloads por pregunta** que solo viven en yoda (64 MB de E5; los de web están además gitignored): sin ellos no se pueden re-puntuar las celdas.
6. **Registrar el entorno de ejecución** (versión de llama.cpp, ROCm, kernel, límite de potencia de 119 W que un coordinador mencionó pero que ningún archivo commiteado confirma) con `rocm-smi` en yoda: es lectura, no GPU de cómputo (ítem 14 del borrador).
7. **Cerrar el hueco de la tabla de costos** (§6 del paper: solo hay latencia; VRAM y tasa de contaminación («taint») no se registraron).

### C. Rehacer o ampliar (cuesta GPU y/o presupuesto; ninguno se hace sin tu OK)
| qué | por qué | costo |
|---|---|---|
| **Segunda semilla de E5** (2B L1, L0, y sus celdas) | hoy una semilla por brazo; H9c y H9a dependen de ella | medido: entrenamientos 2B 1.425 s + 1.399 s; evaluación 2B de las 21 celdas: la ventana completa de las 21 (2B y 0,8B) tardó 2 h 44 min. Total de una semilla extra para 2B solo: **no medido** (estimable sumando los dos números anteriores por la parte de 2B, 63 min de servidor 2B L1 según el registro) |
| **Cambiar la receta para ver si el costo en GSM8K desaparece** (menos pasos, más anclas, otra tasa) | H9c no se cumple y el control se mueve igual; hoy no se sabe si es receta, tasa o anclas | un entrenamiento por variante (~16–24 min cada uno, medidos) + GSM8K de 500 problemas ~13 min por celda (medido: 779 s) + el gate. Es una ablación nueva; se pre-registra antes |
| **Gate «escala 0 = base» sobre Q8_0 mismo** | la fila de transferencia Q8_0 no pasó el gate sobre su propia base | corre 20 ítems; costo no medido, pequeño |
| **Relación retenida de otro tipo, o otra herramienta** | director/screenwriter están cerca de relaciones entrenadas; solo `father` es limpia | un reentrenamiento (L1 y L0) con una nueva extracción de relaciones retenidas, decidida antes de entrenar |
| **Celda web 27B completa y 2B/4B; control `web+recall-empty`** | cerrar H6 y completar la curva web | **presupuesto de búsqueda escrito y aprobado por vos**: ≈ 2.100 búsquedas para el 27B (medido: 2,2 búsquedas por pregunta con búsqueda), costo por búsqueda no medido |
| **Variantes C (27B) y D (2B)** | cerrar la tabla de redacciones | extrapolado 2,3–2,6 h por el 27B; no medido |
| **Más preguntas para H5a** | para poder probar equivalencia ±2 puntos | otra muestra mayor; cuánto no se calculó |

### D. Decisiones tuyas (alcance del paper)
1. **E2, E3, E4, E1b (evaluación), E6, E7:** ¿se corren o se declaran fuera de alcance? Sin E7 el paper no puede contestar el título; hay dos opciones coherentes: (i) correr E7 (hay que diseñar y construir la batería de habilidades, y es trabajo nuevo de Opus + Sonnet, no medido) o (ii) acotar el título y el resumen a «cuánto modelo hace falta *para saber hechos*» y dejar E7 como trabajo futuro. **Mi recomendación es (ii)** si querés publicar pronto en arXiv, porque lo medido ya sostiene un paper completo y honesto en esos términos; (i) es lo que habilita el titular fuerte.
2. **H6 / write-back:** sacarlo del paper o correr el control.
3. **Si E5 alcanza como «resultado de entrenamiento»** o si se espera la segunda semilla antes de escribirlo.
4. **Cómo tratar el costo de GSM8K:** si se reporta como está o se hace la ablación de receta.
5. **Presupuesto de búsqueda** (para cualquier celda web nueva).
6. **¿Dónde publicar?** el plan dice: arXiv primero, después un workshop (NeurIPS/ICLR sobre memoria, agentes o modelos eficientes) con resultados de solo inferencia; COLM si E7 aterriza claro; un congreso principal probablemente quiera E5 más robusto.

---

## 8. Errores del proceso y sus costos (para el apartado de transparencia)

El borrador los cuenta en §4.7; los resumo porque son lo más útil para repetir el protocolo:
1. **Modelo mal identificado en E0** — «Qwen3.8 9B» resultó ser una destilación de la comunidad; se perdieron ≈ 6 de ≈ 12 h de GPU. No lo atrapó el protocolo (el flag `--expect-model` comparaba un nombre). Reparación: `models.lock` con sha256 y un gate en el runner.
2. **Una verificación circular presentada como validación** (la regex de abstención «50/50») — se retiró; tus 50 etiquetas a mano dieron 49/50.
3. **Una celda web inválida en silencio** — créditos agotados, errores 402 convertidos en texto; una auditoría de payloads lo detectó; el harness ahora corta ante cualquier búsqueda fallida.
4. **Contradicciones no reparadas en la línea original** del cuaderno (es solo-anexar por convención; hay que leerlo hasta el final).
5. **Slips de proceso**, declarados por las propias sesiones.
6. **Un test de GPU tumbó producción** (2026-11-20): se corrió el LMLM con el 27B cargado, el OOM del kernel mató `funes`, los dos bridges de WhatsApp y el monitor; la máquina quedó sin responder hasta que la reiniciaste a las 14:49. Costo: unas 3 horas de producción caída (11:39–14:49). El tiempo hasta la recuperación completa figura como no verificado.

Límites del protocolo, también del borrador: todas las sesiones son modelos de la misma familia (puntos ciegos compartidos), no hay revisión humana independiente del código (los únicos datos de etiquetado independiente son tus 50), la autoría por sesión no está en git (los 55 commits llevan tu identidad; solo la cabecera del NOTEBOOK dice quién fue cada uno), y los briefs y mensajes entre sesiones no están archivados.

---

## 9. Qué haría yo, en orden

1. **Ahora, sin GPU (B1–B7):** analizar `funes`, completar `retrieval_hit`, el intervalo de H1, el respaldo de payloads, el registro del entorno y la lectura de los papers. Es lo que más acerca el borrador a una versión enviable y casi no cuesta.
2. **Decidí el alcance (D1):** si vamos con (ii) —título y resumen centrados en «cuánto modelo hace falta para saber hechos»—, el paper queda cerrable con lo que hay más (B). Si vamos con (i), el próximo trabajo es diseñar la batería de habilidades de Funes.
3. **Después, con tu OK, si querés robustecer E5:** gate sobre Q8_0 (barato), luego segunda semilla; la ablación de receta solo si te importa el costo en GSM8K.
4. **Web:** nada se hace hasta que haya un presupuesto aprobado.

*Corregirme si algún dato de este informe no coincide con tu memoria de lo que se hizo: copié de las tablas y del borrador y solo medí por mi cuenta las dos cosas que se indican arriba.*
