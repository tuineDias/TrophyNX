# Custom Achievements Guide

Este guia explica como criar listas de conquistas personalizadas para o TrophyNX no Nintendo Switch.

## Como funciona

O TrophyNX carrega arquivos `.json` de `sdmc:/switch/TrophyNX/games/`. Cada arquivo define conquistas para um jogo via seu `title_id`.

## Dois tipos de conquistas

- **Estatísticas de jogo** (horas, lançamentos) — funciona para qualquer jogo
- **Baseado em save** — lê valores dos saves do jogo usando leitores como `json`, `gvas`, `nrbf`, `byml`, `binary_le`

## Início rápido

Use **ZR** no TrophyNX para gerar `scan_report.txt` com o `title_id` do seu jogo e caminhos dos arquivos de save. Copie um rascunho do `drafts/` do GitHub ou crie um JSON mínimo:

```json
{
  "title_id": "0100535012974000",
  "name": "Hades",
  "achievements": [
    { "id": "h10", "name": "10 Horas", "rule": { "type": "playtime_hours", "target": 10 } }
  ]
}
```

## Regras baseadas em save

Usam caminhos como `playerData/geo` ou `ShineData[*]/IsGet` com tipos: `flag`, `count`, `sum`.

## Testando

Feche o jogo → abra TrophyNX → pressione **Y** → pressione **X** para valores de debug.

## Compartilhe listas funcionais

Via GitHub issue ou botão "🌐 Share" do Save Lab.