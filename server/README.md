# Assistant Server

Бэкенд персонального голосового ассистента на Seeed Sticky (reTerminal E1005):
прибор на холодильнике присылает голосовой запрос, ассистент выполняет его и
отвечает текстом на экран прибора.

Пока это прототип: он принимает WAV с прибора, кладёт его на диск и отвечает
фиксированной фразой.

## Запуск

```sh
uv sync
uv run python main.py
```

При старте сервер печатает URL, который нужно вписать в прибор.

## Документация

- [docs/vision.md](docs/vision.md) — замысел, прибор, принятые решения, сценарии
- [docs/device-contract.md](docs/device-contract.md) — контракт `POST /audio` с прошивкой
- [docs/server.md](docs/server.md) — как устроен сервер сейчас: запуск, эндпоинты, намеренные сбои
- [docs/use-cases/shopping-list.md](docs/use-cases/shopping-list.md) — список покупок
- [docs/open-questions.md](docs/open-questions.md) — что ещё не решено
