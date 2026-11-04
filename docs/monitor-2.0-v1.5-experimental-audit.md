# Cemu-CRF v1.5 — correções experimentais da auditoria DeepSeek

Branch: `blackOps-Branch`

## Alterações aplicadas nesta etapa

### 1. Alocação de localizações XFB sem sobreposição

Arquivo: `src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitGLSLHeader.hpp`

- A geração de saídas dummy do Vertex Shader agora fica limitada à quantidade real de entradas da tabela de imports do Pixel Shader (até o limite da tabela local), em vez de declarar 32 localizações incondicionalmente.
- A localização inicial dos blocos de transform feedback deixa de ser sempre zero.
- Sem Geometry Shader, a reserva começa após as localizações da interface VS/PS.
- Com Geometry Shader, a reserva começa após os parâmetros VS→GS ou após a maior localização GS→PS emitida, conforme o estágio.
- O objetivo é eliminar a colisão observada no Monitor 2.0: `location = 0` em `XfbBlock` sobrepondo uma saída já declarada.

Limitação: ainda precisa ser validado em compilação e em runtime, especialmente em dispositivos com limites baixos de saídas por estágio.

### 2. Medição de espera por shaders e criação de pipelines

Arquivo: `src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.cpp`

- Mede o tempo gasto aguardando/preparando os módulos de shader antes da criação do pipeline.
- Mede separadamente a duração de `vkCreateGraphicsPipelines`.
- Registra eventos que ultrapassam 5 ms, incluindo hashes VS/PS/GS, thread de renderização, disponibilidade de `VK_EXT_pipeline_creation_cache_control`, resultado Vulkan e tentativas por falta de memória.
- Não finge habilitar compilação assíncrona de pipelines quando a extensão necessária não existe. O comportamento síncrono existente foi preservado; agora o log poderá mostrar onde está o custo.

### 3. Diagnóstico da classificação RGBA8 como profundidade

Arquivo: `src/Cafe/HW/Latte/Core/LatteTexture.cpp`

- Quando `LatteTexture_CreateMapping` recebe `isDepth=true` junto com `R8_G8_B8_A8_UNORM`, registra endereço, dimensões, pitch, tile mode e dimensões da view.
- O aviso é limitado às primeiras oito ocorrências para evitar inundar o log.
- A classificação não é alterada automaticamente: `isDepth` é um parâmetro independente e mudar isso sem rastrear o chamador poderia quebrar views legítimas.

## Sugestões deliberadamente não alteradas

### 4. Buffer circular de Readback de 32 MiB

A política de reutilização ainda não foi alterada. O código possui uma fila FIFO de readbacks e cada item tem um ID de command buffer, `IsFinished()` e `ForceFinish()`. O índice circular é gerenciado em `VulkanRenderer::texture_createReadback`.

- Foi adicionado um aviso limitado `[ReadbackRing]` quando o índice volta ao início do buffer de 32 MiB.
- Esse aviso mede a ocorrência de wrap, mas não prova nem impede uma sobreposição com dados ainda aguardando consumo.

Uma correção segura exige ligar a reserva do intervalo à liberação do item da fila. Simplesmente esperar o device idle não basta: uma transferência pode ter terminado e o resultado ainda estar aguardando processamento pela CPU.

### 5. BC4/BC5 SNORM

Nenhuma alteração matemática foi feita. A implementação já diferencia saídas assinadas e não assinadas. A sugestão de alterar a normalização do DeepSeek não foi confirmada e poderia introduzir corrupção de dados.

## Testes necessários

1. Compilar o APK sem iniciar automaticamente o workflow pelo assistente.
2. Carregar Black Ops 2 Zombies com compilação assíncrona ativada.
3. Verificar se desaparecem os erros GLSL `overlapping use of location 0`.
4. Verificar se corpos do jogador e dos zumbis aparecem.
5. Disparar armas e observar travamentos.
6. Exportar Monitor 2.0 e procurar por `[PipelineTiming]` e `[TextureClassification]`.

## Estado

Estas alterações são experimentais e ainda não foram compiladas nem testadas em runtime nesta sessão. A auditoria do Readback e qualquer mudança BC4/BC5 permanecem pendentes por segurança.
