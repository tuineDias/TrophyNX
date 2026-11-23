# Guia de Dumping

Este guia explica como extrair arquivos de jogo de um Switch com Atmosphère CFW usando **nxdumptool** para dois jogos do TrophyNX:

## THEATRHYTHM FINAL BAR LINE

Dump `global-metadata.dat` (~12 MB) do RomFS da aplicação base. O TrophyNX auto-encontra a senha de descriptografia do save e armazena em `sdmc:/switch/TrophyNX/keys/theatrhythm.txt`.

## DELTARUNE

Dump quatro arquivos `game.win` (capítulos 1–4) do Patch RomFS da atualização, depois faça patch no PC usando **UndertaleModTool CLI** e o script `patch.ps1` do TrophyNX. Copie a pasta `atmosphere` resultante para a raiz do SD para habilitar gravação de troféus via LayeredFS.

## Pré-requisitos

- `prod.keys` (via Lockpick_RCM)
- `nxdt_rw_poc.nro` no SD
- Forma de transferir arquivos para DELTARUNE

Abra nxdumptool segurando **R** ao lançar um jogo para acesso total à memória.