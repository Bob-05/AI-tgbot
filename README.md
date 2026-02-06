# AI-tgbot

📘 Book RAG Assistant — интеллектуальный помощник по книге
Этот проект реализует RAG-систему (Retrieval-Augmented Generation) для Telegram-бота, который отвечает на вопросы пользователей по книге «Путь наименьшего сопротивления» Роберта Фритца.

🧩 Архитектура:
 - Загрузка и предобработка текста — чтение, очистка, чанкование с перекрытием.
 - Векторизация — получение эмбеддингов через Yandex Cloud Embeddings API.
 - Семантический поиск — косинусное сходство между запросом и чанками.
 - Генерация ответа — формирование промпта и запрос к Yandex GPT.
 - Telegram-интерфейс — общение с пользователем через TgBot.

🔧 Стек:
 - Язык: C++17
 - Библиотеки: TgBot, nlohmann/json, libcurl
 - AI-бэкенд: Yandex Cloud AI (GPT и Embeddings)

📌 Ключевые возможности:
 - Автоматическая обработка больших текстов
 - Резервный текстовый поиск при недоступности эмбеддингов
 - Поддержка длинных ответов (>4000 символов)
 - Детальное логирование и отладка

----------------------------------------------Anglish----------------------------------------------
📘 Book RAG Assistant — an intelligent book assistant
This project implements a RAG system (Retrieval-Augmented Generation) for a Telegram bot that answers users' questions about the book "The Path of Least Resistance" by Robert Fritz.

🧩 Architecture:
 - Loading and preprocessing of text — reading, cleaning, chunking with overlap.
 - Vectorization — getting embeddings via the Yandex Cloud Embeddings API.
 - Semantic search — cosine similarity between a query and chunks.
 - Response generation — creating a prompt and a request to Yandex GPT.
 - Telegram interface — communication with the user via TgBot.

🔧 Stack:
 - Language: C++17
 - Libraries: TgBot, nlohmann/json, libcurl
 - AI backend: Yandex Cloud AI (GPT and Embeddings)

📌 Key features:
 - Automatic processing of large texts
 - Backup text search when embeddings are unavailable
 - Support for long responses (>4000 characters)
 - Detailed logging and debugging
