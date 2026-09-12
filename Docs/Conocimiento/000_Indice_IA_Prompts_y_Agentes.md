---
tipo: moc
resumen: IngenierÃ­a de prompts, System Prompts, RAG, herramientas (tools) LLM y reglas de agentes IA.
fecha_creacion: 2026-09-11
fecha_modificacion: 2026-09-11
related: []
estado: activo
tags:
  - moc
  - agentes
  - ia
---

# 000 Indice IA Prompts y Agentes

> [!NOTE]
> IngenierÃ­a de prompts, System Prompts, RAG, herramientas (tools) LLM y reglas de agentes IA.

---

## Notas Vinculadas

``dataview
TABLE resumen as "Resumen", fecha_modificacion as "Modificado"
FROM "Docs/Conocimiento"
WHERE tipo = "agentes" OR contains(tags, "prompts")
SORT fecha_modificacion DESC
``