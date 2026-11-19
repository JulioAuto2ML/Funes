# Notas: arXiv 2510.17934 — AtlasKV (Opus-audit, 2026-11-19)

Rol diseño/auditoría: nada ejecutado, nada tocado salvo este archivo. Leído: el PDF completo v2 (ICLR 2026, 26 págs.) con `pdftotext`; leí abstract, §1–6, Tabla 1–4, Fig. 4–5, A.1, J. **No leí** los apéndices B–I (ablaciones extra, derivaciones, entrenamiento dinámico) salvo las líneas que cito. WebFetch no pudo decodificar el PDF; usé la copia local. Todo lo marcado «(memoria)» no está verificado.

## 1. Qué afirma el paper

Huang, Tsang, Bai, Peng, Zhang, Song (HKUST/Huawei). **AtlasKV**: inyecta un grafo de conocimiento (tripletas) en un LLM *dentro de las capas de atención* (KV paramétrico, al estilo KBLaM), sin retriever externo, sin contexto largo y sin reentrenar al cambiar de KG. Dos piezas:
- **KG2KV** (§4.1): cada tripleta (h,r,t) se enmascara en h o t y se reescribe como par Q-K-V de texto; datos de entrenamiento más diversos: «diversity ratio» 7.864 % vs 0.003 % del método sintético de KBLaM, coste 165.7 vs 349.9 tokens (Tabla 1).
- **HiKVP** (§4.2): claves en 3 niveles (UMAP+GMM), poda jerárquica raíz→intermedio→hoja; complejidad O(∛M) (Tabla 2). Fig. 4: <20 GB VRAM para 10⁹ tripletas; KBLaM >40 GB ya con 10⁵; ICL >48 GB con >100 tripletas (p. 9).
- Resultados (Tabla 3): «ACC@1/ACC@5» = ¿la atención del KG en la capa 15 cae sobre la tripleta correcta? (no es exactitud de QA). Ej. ATLAS-Pes2o-QKV, 10² tripletas: KBLaM 16.4/45.5 vs AtlasKV 87.3/92.7; a 10⁴: 0.0/3.6 vs 16.4/49.0 (con HiKVP; 47.3/67.2 sin poda). Fig. 5: GPTScore (GPT-4o) mejor que KBLaM; ICL >0.9 pero inviable en memoria.
- Un solo modelo: **Llama-3.1-8B-Instruct**, una GPU de 48 GB, bf16 (Repro., A.1). Entrenan con 20K tripletas, 3K pasos. Codificadores MiniLM y text-embedding-3-large.
- Limitaciones propias (App. J): tripletas independientes (sin multi-salto), pérdida de precisión por la poda, coste CPU↔GPU.

## 2. Relación con H1–H8 / E0–E7

El paper es **ingeniería de inyección de conocimiento a escala**, no un estudio de qué hacen los parámetros. No mide tamaño de modelo, ni habilidad vs conocimiento, ni PopQA/GSM8K, ni conflictos, ni abstención.
- **H1, H2, H7, E1, E7 (curva de tamaño, piso de habilidad):** sin solape ni evidencia. Un solo tamaño (8B). No aporta baseline numérico.
- **H3 / E2 (conflictos, ediciones):** sin datos. Solo el argumento de diseño de que se adapta «sin reentrenar» a un KG nuevo; no prueba qué gana si el KG contradice los pesos. No lo leí en apéndices (no verificado).
- **H5 / E3 (cuándo buscar, abstención):** no mide alucinación ni abstención. Dato relacionado débil: los ACC@k bajan con el tamaño del KG (a 10⁴ solo 3.6 % ACC@5 en KBLaM), o sea, el ruido de recuperación es real en este paradigma; consistente con que E3 mida «tienda ruidosa».
- **E4 (forma del store):** el único contacto real. Es un tercer eje: store *paramétrico en atención* vs. nuestro vectores+FTS+links vs. tripletas exactas. Su HiKVP es una poda jerárquica sub-lineal sobre claves, parecida en espíritu a «cambio adaptativo por densidad» (KO). No es comparable directo: requiere entrenar W̃Q/W̃K/W̃V y acceso a las capas, y **no corre en llama.cpp/GGUF** (inferencia: inferido, no verificado). No entra en el plan actual.
- **E5 (LoRA «aprende a consultar»):** relacionado por contraste: ellos entrenan *cabezas de atención* para leer el KG; nosotros una LoRA que aprende a *llamar* una herramienta. Sirve como trabajo relacionado de «parametric KG injection».
- **H4/E1b/E6:** nada. No enmascara hechos en preentrenamiento ni ablaciona capas. Única nota para E6: usan atención de la capa 15 como señal de «grounding» (App. A.2, no leído en detalle) — pista de que hay localización por capa, pero con un modelo 8B; no es evidencia nuestra.
- **Métrica a no adoptar:** GPTScore con GPT-4o (Fig. 11, prompt de scoring) choca con nuestra regla de exact/alias + juez fijo validado con etiquetas humanas. ACC@k de atención no es QA.

## 3. Novedad y posicionamiento

**No amenaza** la contribución («mismo agente, mismos modelos y tareas, hechos en pesos / store local / web; coste en precisión, VRAM, latencia, taint»). AtlasKV: un tamaño, sin agente, sin web, sin condición «cerrado vs store» por tamaño, sin habilidad vs conocimiento, sin seguridad. Sí es un vecino por la pregunta «cuánta VRAM cuesta llevar hechos»: ellos responden con arquitectura (KV paramétrico), nosotros con tienda externa y un barrido de tamaño. Posicionar en Related Work como *paramétrico-en-atención (KBLaM, AtlasKV)* frente a *no paramétrico (RAG, nuestro store/web)* frente a *pesos enmascarados (LMLM, Co-LMLM)*: tres lugares donde puede vivir un hecho; nosotros medimos el reparto en un agente desplegado. Dos riesgos de redacción: (a) no afirmar «nadie compara dónde vive el conocimiento» sin matizar que AtlasKV/KBLaM comparan ICL vs paramétrico en VRAM y precisión de grounding; (b) su argumento de VRAM (ICL no escala) es un punto de apoyo para nuestro costo de contexto, no una competencia.

## 4. Acciones (máx. 5)

1. **Citar** AtlasKV (+ KBLaM, arXiv 2410.10450 (memoria, sin verificar)) en Related Work, párrafo «parametric knowledge injection». *Solo diseño.*
2. **Reformular la frase de novedad** del plan/paper para que diga lo que nadie hace *en un agente con modelos de tamaño variable*, y no «nadie compara dónde vive el conocimiento». *Solo diseño* (el coordinador edita el plan; yo no).
3. **No adoptar** ACC@k de atención ni GPTScore; mantener contains/exact y juez validado. Una línea en Limitaciones: «no probamos KV paramétrico». *Solo diseño.*
4. **Leer App. B.4.1, E y A.2 de este mismo paper** (top-k, dinámica de entrenamiento, por qué capa 15) si E6 se hace; y KBLaM (original) para decidir si «store paramétrico» merece un brazo exploratorio. *Solo lectura.*
5. **Opcional, post-E1:** brazo exploratorio «store paramétrico (KBLaM/AtlasKV)» en E4 con Llama-3.1-8B. Probablemente fuera de alcance: requiere entrenar y código en PyTorch (ROCm), no GGUF. *Necesitaría código + GPU; recomiendo no hacerlo* salvo que un revisor lo pida.

Prioridad: 1 y 2 antes del borrador; 3 al escribir Limitaciones; 4–5 solo si E4/E6 se activan.
