# Cemu-CRF v1.5 — auditoria experimental do Monitor 2.0

Branch de destino: `v1.5`

## Alterações portadas ou complementadas

### 1. Interface Vertex Shader / Transform Feedback (XFB)

Arquivo: `src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitGLSLHeader.hpp`

- A geração das saídas dummy do Vertex Shader agora fica limitada à quantidade real de entradas da tabela de imports do Pixel Shader (até o limite de 32 localizações).
- Para Vertex Shader sem Geometry Shader, o início das localizações XFB é reservado depois das localizações da interface VS/PS.
- Para Vertex Shader com Geometry Shader, é mantida a reserva baseada em `ringParameterCount`.
- Para Geometry Shader, a reserva considera as localizações exportadas pelo copy shader e ignora índices fora do limite esperado.
- O objetivo é eliminar colisões como `overlapping use of location 0` entre saídas GLSL e blocos XFB.

**Atenção:** esta é uma correção experimental de layout de interface. Precisa ser validada em compilação e em runtime, inclusive em shaders com e sem Geometry Shader. Não é possível afirmar ainda que todos os erros de textura invisível sejam causados por esse problema.

### 2. Diagnóstico do tempo de preparação de shaders

Arquivo: `src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.cpp`

- Adicionada medição do tempo gasto verificando/preparando os módulos de shader e solicitando compilação síncrona quando necessário.
- Eventos acima de 5 ms geram linhas `[PipelineTiming] Shader readiness/compilation wait`, com hashes VS/PS/GS, thread de renderização e disponibilidade de `VK_EXT_pipeline_creation_cache_control`.
- A medição já existente de `vkCreateGraphicsPipelines` foi preservada. Ela continua registrando o tempo de criação do pipeline separadamente.
- Nenhuma promessa de compilação assíncrona foi adicionada: quando a extensão não está disponível, o comportamento síncrono existente continua.

Isso permite distinguir espera pela compilação dos shaders de tempo gasto na criação do pipeline Vulkan.

### 3. Classificação de textura RGBA8 / depth

Arquivo: `src/Cafe/HW/Latte/Core/LatteTexture.cpp`

A branch `v1.5` já continha um diagnóstico próprio mais específico para o caso observado (formato GX2 `0x001a` solicitado como depth), incluindo endereço, dimensões, pitch, tile mode e caller. Ele foi preservado, sem substituição pelo aviso genérico da outra branch e sem alterar a classificação da textura.

### 4. Readback ring de 32 MiB

Arquivo: `src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.cpp`

A branch `v1.5` já continha um aviso `[Monitor 2.0] ReadbackRing wrap` com offset, tamanho da transferência, capacidade e command buffer. Essa implementação foi preservada.

O aviso registra o wrap, mas **não prova nem impede** uma sobreposição com dados ainda aguardando consumo pela CPU. A política de reutilização do buffer não foi alterada; uma correção segura depende de auditar o ciclo de vida dos itens da fila de readback.

### 5. BC4 / BC5 SNORM

Nenhuma alteração matemática foi feita. A implementação já diferencia saídas assinadas e não assinadas; a sugestão de mudar a normalização não foi confirmada e poderia introduzir corrupção.

## Plano de teste da v1.5

1. Compilar a branch `v1.5` manualmente.
2. Carregar Black Ops 2 Zombies com compilação assíncrona ativada.
3. Verificar se os erros GLSL `overlapping use of location 0` diminuem ou desaparecem.
4. Verificar se os corpos do jogador e dos zumbis aparecem.
5. Disparar armas e observar travamentos.
6. Exportar o Monitor 2.0 e procurar por:
   - `[PipelineTiming] Shader readiness/compilation wait`
   - `[Monitor 2.0] PipelineCreate`
   - `[Monitor 2.0] Unexpected depth classification`
   - `[Monitor 2.0] ReadbackRing wrap`

## Estado

As alterações XFB e de medição de preparação de shaders foram gravadas na branch `v1.5`, mas ainda **não foram compiladas nem testadas em runtime** nesta etapa.

Não foi iniciado nenhum workflow. A auditoria de sincronização/reutilização do readback ring permanece pendente. Nenhuma correção de BC4/BC5 foi aplicada.
