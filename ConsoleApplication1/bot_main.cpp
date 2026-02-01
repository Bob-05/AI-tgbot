#include <tgbot/tgbot.h>
#include <curl/curl.h>
#include <iostream>
#include <string>
#include <fstream>
#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>
#include <mutex>
#include <vector>
//#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstdlib>
//#include <memory>
//#include <cctype>

// Вместо Windows-заголовков в начале файла
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

//кроссплатформенная ф-я
void mySleep(int milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    usleep(milliseconds * 1000);
#endif
}



using namespace std;
using namespace TgBot;
using json = nlohmann::json;

// ========== ВАШИ ДАННЫЕ ==========
const string BOT_TOKEN = "8537200045:AAHl1R9_QGMLfVyOq8m7dfUd3n0ofALIaE8";
const string BOOK_FILE = "book.txt";
const string AUTHOR_FILE = "author.txt";

// Yandex Cloud API
const string FOLDER_ID = "b1gn41ejchr0jsrbtuo2";
const string API_KEY = "AQVN0I6rC0xuMtV2Bm-mj-4UWS5hibL_Nezk3pwd";

// API URLs
const string YANDEX_GPT_URL = "https://llm.api.cloud.yandex.net/foundationModels/v1/completion";
const string YANDEX_EMBEDDING_URL = "https://llm.api.cloud.yandex.net/foundationModels/v1/textEmbedding";

// ========== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ==========
atomic<bool> yandexApiAvailable(false);
mutex consoleMutex;
mutex dataMutex;
vector<pair<string, vector<float>>> bookChunks;

// ========== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ==========
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, string* response) {
    size_t totalSize = size * nmemb;
    response->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

string readFile(const string& filename) {
    ifstream file(filename, ios::binary);
    if (!file.is_open()) {
        cerr << "❌ Не удалось открыть файл: " << filename << endl;
        return "";
    }

    // Проверяем размер файла
    file.seekg(0, ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, ios::beg);

    cout << "📄 Размер файла " << filename << ": " << fileSize << " байт" << endl;

    if (fileSize == 0) {
        cerr << "❌ Файл пуст: " << filename << endl;
        return "";
    }

    // Читаем файл как бинарный
    string content(fileSize, '\0');
    file.read(&content[0], fileSize);
    file.close();

    // Убираем BOM если есть (UTF-8 BOM: EF BB BF)
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content = content.substr(3);
    }

    cout << "✅ Файл прочитан: " << content.size() << " символов" << endl;
    return content;
}

// Функция для очистки текста от недопустимых символов
string cleanText(const string& text) {
    string result = text;

    // Удаляем нулевые байты и управляющие символы (кроме табуляции и новой строки)
    result.erase(std::remove_if(result.begin(), result.end(),
        [](char c) {
            return c == '\0' || (c >= 0x00 && c <= 0x1F && c != '\t' && c != '\n' && c != '\r');
        }),
        result.end());

    // Убираем лишние пробелы в начале и конце
    size_t start = result.find_first_not_of(" \t\n\r");
    size_t end = result.find_last_not_of(" \t\n\r");

    if (start != string::npos && end != string::npos) {
        result = result.substr(start, end - start + 1);
    }
    else {
        result.clear();
    }

    return result;
}

vector<string> splitIntoChunks(const string& text, int maxChunkSize = 1500) {
    vector<string> chunks;

    if (text.empty()) {
        cout << "⚠️ Текст для разделения пуст" << endl;
        return chunks;
    }

    string cleanTextStr = cleanText(text);
    if (cleanTextStr.empty()) {
        cerr << "❌ Текст стал пустым после очистки" << endl;
        return chunks;
    }

    cout << "✂️  Начинаю разделение текста на чанки..." << endl;
    cout << "📊 Исходный размер текста: " << cleanTextStr.size() << " байт" << endl;

    size_t textLength = cleanTextStr.size();
    size_t pos = 0;
    int chunkCount = 0;

    // Безопасная функция проверки пробела
    auto isSafeSpace = [](unsigned char c) -> bool {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        };

    while (pos < textLength) {
        size_t chunkSize = min((size_t)maxChunkSize, textLength - pos);
        size_t endPos = pos + chunkSize;

        // Если не конец текста, ищем хорошую границу
        if (endPos < textLength) {
            // Ищем последний пробел или новую строку в пределах чанка
            size_t goodBreakPos = string::npos;

            // Сначала ищем новую строку
            for (size_t i = endPos - 1; i > pos && i > 0; i--) {
                if (cleanTextStr[i] == '\n') {
                    goodBreakPos = i + 1; // Включаем новую строку
                    break;
                }
            }

            // Если не нашли новую строку, ищем пробел
            if (goodBreakPos == string::npos) {
                for (size_t i = endPos - 1; i > pos && i > 0; i--) {
                    unsigned char c = static_cast<unsigned char>(cleanTextStr[i]);
                    if (c == ' ') {
                        goodBreakPos = i;
                        break;
                    }
                }
            }

            // Если нашли хорошую границу и она не слишком близко к началу
            if (goodBreakPos != string::npos && goodBreakPos > pos + maxChunkSize / 3) {
                endPos = goodBreakPos;
            }
        }

        // Вырезаем чанк
        string chunk = cleanTextStr.substr(pos, endPos - pos);

        // Очищаем чанк
        chunk = cleanText(chunk);

        if (!chunk.empty()) {
            chunks.push_back(chunk);
            chunkCount++;

            if (chunkCount % 20 == 0) {
                cout << "📊 Создано чанков: " << chunkCount << endl;
            }
        }

        // Перемещаем позицию
        pos = endPos;

        // Пропускаем пробелы в начале следующего чанка (безопасно)
        while (pos < textLength) {
            unsigned char c = static_cast<unsigned char>(cleanTextStr[pos]);
            if (isSafeSpace(c)) {
                pos++;
            }
            else {
                break;
            }
        }
    }

    cout << "✅ Разделение завершено! Создано " << chunks.size() << " чанков" << endl;

    if (!chunks.empty()) {
        cout << "📊 Средний размер чанка: " << (cleanTextStr.size() / chunks.size()) << " байт" << endl;
    }

    return chunks;
}

// Получение эмбеддинга с обработкой ошибок
vector<float> getEmbedding(const string& text, int retryCount = 5) {
    string cleanedText = cleanText(text);

    if (cleanedText.empty()) {
        cerr << "⚠️ Текст пуст после очистки" << endl;
        return {};
    }

    cout << "🔍 Запрос эмбеддинга для текста (" << cleanedText.size() << " символов)" << endl;

    for (int attempt = 0; attempt < retryCount; attempt++) {
        CURL* curl = curl_easy_init();
        string response;
        vector<float> embedding;

        if (!curl) {
            cerr << "❌ Не удалось инициализировать CURL" << endl;
            continue;
        }

        try {
            json request = {
                {"modelUri", "emb://" + FOLDER_ID + "/text-search-query/latest"},
                {"text", cleanedText},
                {"embeddingType", "EMBEDDING_TYPE_DOCUMENT"}
            };

            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            string authHeader = "Authorization: Api-Key " + API_KEY;
            headers = curl_slist_append(headers, authHeader.c_str());

            curl_easy_setopt(curl, CURLOPT_URL, YANDEX_EMBEDDING_URL.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

            // ИСПРАВЛЕНИЕ: Используем error_handler_t::replace для избежания UTF-8 ошибок
            string requestBody = request.dump(-1, ' ', false, json::error_handler_t::replace);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

            CURLcode res = curl_easy_perform(curl);

            if (res == CURLE_OK && !response.empty()) {
                response.erase(std::remove(response.begin(), response.end(), '\0'), response.end());

                if (response.empty()) {
                    cerr << "❌ Ответ API пуст после очистки" << endl;
                    continue;
                }

                try {
                    json jsonResponse = json::parse(response);

                    if (jsonResponse.contains("error")) {
                        string errorMsg;

                        if (jsonResponse["error"].is_string()) {
                            errorMsg = jsonResponse["error"].get<string>();
                        }
                        else if (jsonResponse["error"].is_object()) {
                            if (jsonResponse["error"].contains("message")) {
                                errorMsg = jsonResponse["error"]["message"].get<string>();
                            }
                            else if (jsonResponse["error"].contains("details")) {
                                errorMsg = "Ошибка с деталями";
                            }
                        }

                        cerr << "❌ Ошибка API Yandex: " << errorMsg << endl;

                        if (errorMsg.find("model") != string::npos ||
                            errorMsg.find("uri") != string::npos) {
                            cerr << "⚠️ ВАЖНО: Проверьте корректность modelUri в консоли Yandex Cloud" << endl;
                            cerr << "⚠️ Текущий modelUri: emb://" + FOLDER_ID + "/text-search-query/latest" << endl;
                        }
                    }
                    else if (jsonResponse.contains("embedding")) {
                        embedding = jsonResponse["embedding"].get<vector<float>>();

                        if (!embedding.empty()) {
                            cout << "✅ Получен эмбеддинг размером " << embedding.size() << endl;
                            curl_easy_cleanup(curl);
                            curl_slist_free_all(headers);
                            return embedding;
                        }
                        else {
                            cerr << "⚠️ Эмбеддинг пуст" << endl;
                        }
                    }
                    else if (jsonResponse.contains("result") && jsonResponse["result"].contains("embedding")) {
                        embedding = jsonResponse["result"]["embedding"].get<vector<float>>();

                        if (!embedding.empty()) {
                            cout << "✅ Получен эмбеддинг размером " << embedding.size() << endl;
                            curl_easy_cleanup(curl);
                            curl_slist_free_all(headers);
                            return embedding;
                        }
                    }
                    else {
                        cerr << "❌ Неожиданный формат ответа" << endl;
                    }
                }
                catch (const json::exception& e) {
                    cerr << "❌ Ошибка парсинга JSON: " << e.what() << endl;

                    if (response.find('{') != 0) {
                        cerr << "❌ Ответ не начинается с '{'. Возможно проблема с API" << endl;
                    }
                }
            }
            else {
                cerr << "❌ Ошибка CURL: " << curl_easy_strerror(res)
                    << " (попытка " << (attempt + 1) << "/" << retryCount << ")" << endl;
            }

            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
        }
        catch (const exception& e) {
            cerr << "❌ Исключение в getEmbedding: " << e.what() << endl;
            if (curl) {
                curl_easy_cleanup(curl);
            }
        }

        // Пауза перед повторной попыткой
        if (attempt < retryCount - 1) {
            int delay = (attempt + 1) * 2000;
            cout << "⏳ Пауза " << delay << " мс перед повторной попыткой..." << endl;
            mySleep(delay);
        }
    }

    cerr << "❌ Не удалось получить эмбеддинг после " << retryCount << " попыток" << endl;
    return {};
}

// Косинусное сходство с проверкой
float cosineSimilarity(const vector<float>& a, const vector<float>& b) {
    if (a.size() != b.size() || a.empty() || b.empty()) {
        return 0.0f;
    }

    // Проверка на NaN или бесконечные значения
    for (size_t i = 0; i < a.size(); i++) {
        if (isnan(a[i]) || isinf(a[i]) || isnan(b[i]) || isinf(b[i])) {
            return 0.0f;
        }
    }

    float dot = 0.0f, normA = 0.0f, normB = 0.0f;
    for (size_t i = 0; i < a.size(); i++) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }

    if (normA < 1e-8f || normB < 1e-8f) {
        return 0.0f;
    }

    float similarity = dot / (sqrt(normA) * sqrt(normB));

    // Ограничиваем значения [-1, 1]
    return max(-1.0f, min(1.0f, similarity));
}

// Инициализация RAG системы
bool initRAGSystem() {
    lock_guard<mutex> lock(consoleMutex);
    cout << "🔄 Инициализация RAG системы..." << endl;

    // Проверяем существование файлов
    {
        ifstream bookFile(BOOK_FILE);
        if (!bookFile.good()) {
            cerr << "❌ Файл книги не найден: " << BOOK_FILE << endl;
            return false;
        }
        bookFile.close();
    }

    string bookText = readFile(BOOK_FILE);

    if (bookText.empty()) {
        cerr << "❌ Текст книги пуст" << endl;
        return false;
    }

    // Добавляем информацию об авторе
    string authorText = readFile(AUTHOR_FILE);
    if (!authorText.empty()) {
        bookText = "ИНФОРМАЦИЯ ОБ АВТОРЕ:\n" + authorText + "\n\n" + bookText;
        cout << "✅ Добавлена информация об авторе" << endl;
    }

    // Разбиваем на чанки
    cout << "✂️  Разбиваю текст на чанки..." << endl;
    vector<string> chunks = splitIntoChunks(bookText, 800);

    if (chunks.empty()) {
        cerr << "❌ Не удалось разбить текст на чанки" << endl;
        return false;
    }

    // Очищаем старые данные
    {
        lock_guard<mutex> dataLock(dataMutex);
        bookChunks.clear();
        bookChunks.reserve(chunks.size());
    }

    // Получаем эмбеддинги для каждого чанка
    int successful = 0;
    int failed = 0;

    // Сначала тестируем API на одном чанке
    if (!chunks.empty()) {
        cout << "🧪 Тестируем API на первом чанке..." << endl;
        vector<float> testEmbedding = getEmbedding(chunks[0]);

        if (testEmbedding.empty()) {
            cerr << "❌ Тестовый запрос не удался. Проверьте API ключ и modelUri" << endl;
            cerr << "⚠️  Продолжаем без эмбеддингов (режим fallback)" << endl;

            // Сохраняем все чанки без эмбеддингов
            {
                lock_guard<mutex> dataLock(dataMutex);
                for (const auto& chunk : chunks) {
                    bookChunks.push_back({ chunk, {} });
                }
            }

            cout << "✅ RAG система готова в fallback режиме. Чанков: " << chunks.size() << endl;
            return true;
        }
        else {
            cout << "✅ Тестовый запрос успешен! Размер эмбеддинга: " << testEmbedding.size() << endl;
        }
    }

    // Получаем эмбеддинги для всех чанков
    for (size_t i = 0; i < chunks.size(); i++) {
        cout << "🔍 Получаю эмбеддинг для чанка " << i + 1 << "/" << chunks.size()
            << " (" << chunks[i].size() << " символов)" << endl;

        vector<float> embedding = getEmbedding(chunks[i]);

        if (!embedding.empty()) {
            {
                lock_guard<mutex> dataLock(dataMutex);
                bookChunks.push_back({ chunks[i], embedding });
            }
            successful++;
            cout << "✅ Успешно (всего: " << successful << ")" << endl;
        }
        else {
            failed++;
            cerr << "❌ Не удалось получить эмбеддинг (всего ошибок: " << failed << ")" << endl;

            // Сохраняем текст без эмбеддинга
            {
                lock_guard<mutex> dataLock(dataMutex);
                bookChunks.push_back({ chunks[i], {} });
            }
        }

        // Пауза между запросами
        /*
        if ((i + 1) % 3 == 0) {
            cout << "⏳ Пауза 2 секунды..." << endl;
            mySleep(2000);
        }
        */

        // Периодически выводим статистику
        if ((i + 1) % 20 == 0) {
            cout << "📊 Прогресс: " << (i + 1) << "/" << chunks.size()
                << " (" << ((i + 1) * 100 / chunks.size()) << "%)"
                << " Успешно: " << successful << " Ошибок: " << failed << endl;
        }
    }

    {
        lock_guard<mutex> dataLock(dataMutex);
        cout << "✅ RAG система готова. Всего чанков: " << bookChunks.size()
            << " (с эмбеддингами: " << successful << ")" << endl;

        return !bookChunks.empty();
    }
}

// Поиск релевантных чанков
vector<string> searchRelevantChunks(const string& query, int topK = 5) {
    vector<string> results;

    {
        lock_guard<mutex> dataLock(dataMutex);
        if (bookChunks.empty()) {
            return results;
        }
    }

    // Получаем эмбеддинг запроса
    vector<float> queryEmbedding = getEmbedding(query);

    if (queryEmbedding.empty()) {
        cout << "⚠️ Не удалось получить эмбеддинг запроса, использую текстовый поиск" << endl;
    }

    vector<pair<float, string>> scoredChunks;
    {
        lock_guard<mutex> dataLock(dataMutex);

        // Если есть эмбеддинг запроса, ищем по сходству
        if (!queryEmbedding.empty()) {
            for (const auto& [chunk, embedding] : bookChunks) {
                if (!embedding.empty()) {
                    float score = cosineSimilarity(queryEmbedding, embedding);
                    if (score > 0.3) {
                        scoredChunks.push_back({ score, chunk });
                    }
                }
            }

            // Сортируем по убыванию сходства
            sort(scoredChunks.begin(), scoredChunks.end(),
                [](const pair<float, string>& a, const pair<float, string>& b) {
                    return a.first > b.first;
                });
        }

        // Если не нашли по эмбеддингам, используем текстовый поиск
        if (scoredChunks.empty()) {
            cout << "🔍 Поиск по ключевым словам..." << endl;

            string queryLower = query;
            transform(queryLower.begin(), queryLower.end(), queryLower.begin(), ::tolower);

            vector<string> queryWords;
            stringstream queryStream(queryLower);
            string word;
            while (queryStream >> word) {
                if (word.size() > 2) { // Игнорируем короткие слова
                    queryWords.push_back(word);
                }
            }

            for (const auto& [chunk, embedding] : bookChunks) {
                string chunkLower = chunk;
                transform(chunkLower.begin(), chunkLower.end(), chunkLower.begin(), ::tolower);

                int matches = 0;
                for (const string& queryWord : queryWords) {
                    if (chunkLower.find(queryWord) != string::npos) {
                        matches++;
                    }
                }

                float score = queryWords.empty() ? 0.0f : static_cast<float>(matches) / queryWords.size();
                if (score > 0.3) {
                    scoredChunks.push_back({ score, chunk });
                }
            }

            // Сортируем по количеству совпадений
            sort(scoredChunks.begin(), scoredChunks.end(),
                [](const pair<float, string>& a, const pair<float, string>& b) {
                    return a.first > b.first;
                });
        }
    }

    // Берем topK результатов
    int resultCount = min(topK, (int)scoredChunks.size());
    for (int i = 0; i < resultCount; i++) {
        results.push_back(scoredChunks[i].second);
    }

    cout << "🔍 Найдено релевантных чанков: " << results.size() << endl;
    return results;
}


// ========== УПРОЩЕННЫЙ ПАРСИНГ ОТВЕТА ==========
string extractAnswerFromYandexResponse(const string& rawResponse) {
    if (rawResponse.empty()) {
        cerr << "❌ Пустой ответ от API" << endl;
        return "";
    }

    string response = rawResponse;

    // 1. Удаляем нулевые байты
    response.erase(std::remove(response.begin(), response.end(), '\0'), response.end());

    // 2. Если ответ содержит HTML (ошибка API), возвращаем сообщение
    if (response.find("<!DOCTYPE html>") != string::npos ||
        response.find("<html>") != string::npos ||
        response.find("nginx") != string::npos) {
        cerr << "⚠️ API вернул HTML (возможно ошибка сервера)" << endl;
        return "Временная ошибка сервера Яндекс. Попробуйте позже.";
    }

    // 3. Ищем начало JSON
    size_t jsonStart = response.find('{');
    if (jsonStart == string::npos) {
        jsonStart = response.find('[');
        if (jsonStart == string::npos) {
            cerr << "❌ Не найден JSON в ответе" << endl;

            // Пробуем найти текст напрямую
            size_t textPos = response.find("\"text\":\"");
            if (textPos != string::npos) {
                textPos += 8;
                size_t endPos = response.find('"', textPos);
                if (endPos != string::npos && endPos > textPos) {
                    string answer = response.substr(textPos, endPos - textPos);
                    // Убираем экранирование
                    string cleanAnswer;
                    for (size_t i = 0; i < answer.size(); i++) {
                        if (answer[i] == '\\' && i + 1 < answer.size()) {
                            i++;
                            if (answer[i] == 'n') cleanAnswer += '\n';
                            else if (answer[i] == 't') cleanAnswer += '\t';
                            else cleanAnswer += answer[i];
                        }
                        else {
                            cleanAnswer += answer[i];
                        }
                    }
                    return cleanAnswer;
                }
            }
            return "Ошибка формата ответа от AI.";
        }
    }

    // 4. Обрезаем всё до начала JSON
    if (jsonStart > 0 && jsonStart < response.size()) {
        response = response.substr(jsonStart);
    }

    // 5. Убираем всё после последней закрывающей скобки
    size_t lastBrace = response.rfind('}');
    if (lastBrace != string::npos && lastBrace + 1 < response.size()) {
        response = response.substr(0, lastBrace + 1);
    }

    // 6. Пробуем парсить
    try {
        json jsonResponse = json::parse(response);

        // Проверяем на ошибку API
        if (jsonResponse.contains("error")) {
            string errorMsg = "Ошибка API";
            if (jsonResponse["error"].is_string()) {
                errorMsg = jsonResponse["error"].get<string>();
            }
            else if (jsonResponse["error"].is_object() &&
                jsonResponse["error"].contains("message")) {
                errorMsg = jsonResponse["error"]["message"].get<string>();
            }
            cerr << "❌ Yandex API Error: " << errorMsg << endl;
            return "Ошибка AI сервиса: " + errorMsg;
        }

        // Стандартный путь для YandexGPT
        if (jsonResponse.contains("result") &&
            jsonResponse["result"].contains("alternatives") &&
            jsonResponse["result"]["alternatives"].is_array() &&
            !jsonResponse["result"]["alternatives"].empty()) {

            auto& alt = jsonResponse["result"]["alternatives"][0];
            if (alt.contains("message") &&
                alt["message"].contains("text") &&
                alt["message"]["text"].is_string()) {

                string answer = alt["message"]["text"].get<string>();
                // Простая очистка
                answer.erase(std::remove(answer.begin(), answer.end(), '\0'), answer.end());
                return answer;
            }
        }

        // Альтернативный путь
        if (jsonResponse.contains("alternatives") &&
            jsonResponse["alternatives"].is_array() &&
            !jsonResponse["alternatives"].empty()) {

            auto& alt = jsonResponse["alternatives"][0];
            if (alt.contains("message") &&
                alt["message"].contains("text") &&
                alt["message"]["text"].is_string()) {

                string answer = alt["message"]["text"].get<string>();
                answer.erase(std::remove(answer.begin(), answer.end(), '\0'), answer.end());
                return answer;
            }
        }

        // Прямой текст
        if (jsonResponse.contains("text") && jsonResponse["text"].is_string()) {
            string answer = jsonResponse["text"].get<string>();
            answer.erase(std::remove(answer.begin(), answer.end(), '\0'), answer.end());
            return answer;
        }

        cerr << "⚠️ Не найдено поле с текстом в JSON" << endl;
        return "Не удалось извлечь ответ из данных AI.";

    }
    catch (const json::exception& e) {
        // Если парсинг не удался, пробуем извлечь текст напрямую
        cerr << "⚠️ JSON parse error: " << e.what() << endl;

        // Простой поиск текста в кавычках
        size_t textPos = response.find("\"text\":\"");
        if (textPos == string::npos) textPos = response.find("\"text\": \"");

        if (textPos != string::npos) {
            textPos = response.find('"', textPos + 6) + 1;
            size_t endPos = response.find('"', textPos);

            // Ищем неэкранированную кавычку
            while (endPos != string::npos && endPos > 0 && response[endPos - 1] == '\\') {
                endPos = response.find('"', endPos + 1);
            }

            if (endPos != string::npos && endPos > textPos) {
                string answer = response.substr(textPos, endPos - textPos);
                // Упрощенное удаление экранирования
                string cleanAnswer;
                for (size_t i = 0; i < answer.size(); i++) {
                    if (answer[i] == '\\' && i + 1 < answer.size()) {
                        i++;
                        if (answer[i] == 'n') cleanAnswer += '\n';
                        else if (answer[i] == 't') cleanAnswer += '\t';
                        else if (answer[i] == 'r') cleanAnswer += '\r';
                        else if (answer[i] == '"') cleanAnswer += '"';
                        else if (answer[i] == '\\') cleanAnswer += '\\';
                        else cleanAnswer += answer[i];
                    }
                    else {
                        cleanAnswer += answer[i];
                    }
                }
                cleanAnswer.erase(std::remove(cleanAnswer.begin(), cleanAnswer.end(), '\0'), cleanAnswer.end());
                return cleanAnswer;
            }
        }

        return "Ошибка обработки ответа AI. Попробуйте переформулировать вопрос.";
    }
}

// Генерация ответа
string generateAnswerWithRAG(const string& question) {
    if (question.empty()) {
        return "Пожалуйста, задайте вопрос о книге.";
    }

    cout << "❓ Вопрос: " << question << endl;

    try {
        // Поиск релевантных чанков
        vector<string> relevantChunks = searchRelevantChunks(question, 5);

        if (relevantChunks.empty()) {
            return "В книге не найдено информации по вашему вопросу. "
                "Попробуйте переформулировать вопрос.";
        }

        // Формируем контекст (упрощенный)
        string context;
        for (size_t i = 0; i < relevantChunks.size() && i < 5; i++) {
            string chunk = relevantChunks[i];
            /*
            if (chunk.length() > 600) {
                chunk = chunk.substr(0, 600) + "...";
            }
            */
            context += "Фрагмент " + to_string(i + 1) + ": " + chunk + "\n\n";
        }

        // Ограничиваем общий размер
        /*
        if (context.length() > 2500) {
            context = context.substr(0, 2500) + "...";
        }
        */

        // Промпт
        string prompt =
            "Ты - эксперт по книге 'Путь наименьшего сопротивления' Роберта Фритца.\n"
            "ПРАВИЛА ОТВЕТА:\n"
            "1. Отвечай ТОЛЬКО на основе предоставленных фрагментов книги\n"
            "2. Если в фрагментах нет ответа - честно скажи об этом\n"
            "3. Буть точным: цитируй концепции, используя термины из книги\n"
            "4. Структурируй ответ: ключевая мысль -> объяснение -> пример\n"
            "5. Избегай общих фраз, буть конкретным\n\n"
            "КОНТЕКСТ ИЗ КНИГИ:\n" + context + "\n\n"
            "КВОПРОС ПОЛЬЗОВАТЕЛЯ:\n\n" + question + "\n\n";

        // Запрос к YandexGPT
        CURL* curl = curl_easy_init();
        string response;

        if (!curl) {
            cerr << "❌ Не удалось инициализировать CURL" << endl;
            return "Ошибка подключения.";
        }

        // Упрощенный запрос
        json request = {
            {"modelUri", "gpt://" + FOLDER_ID + "/yandexgpt"},
            {"completionOptions", {
                {"stream", false},
                {"temperature", 0.2},
                {"maxTokens", "1200"}
            }},
            {"messages", {
                {{"role", "user"}, {"text", prompt}}
            }}
        };

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        string authHeader = "Authorization: Api-Key " + API_KEY;
        headers = curl_slist_append(headers, authHeader.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, YANDEX_GPT_URL.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        // КЛЮЧЕВОЕ ИСПРАВЛЕНИЕ: Используем error_handler_t::replace
        string requestBody = request.dump(-1, ' ', false, json::error_handler_t::replace);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
        
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

        CURLcode res = curl_easy_perform(curl);

        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);

        if (res != CURLE_OK) {
            cerr << "❌ CURL Error: " << curl_easy_strerror(res) << endl;
            return "Ошибка соединения с AI. Попробуйте позже.";
        }

        // Используем новую функцию для извлечения ответа
        string answer = extractAnswerFromYandexResponse(response);

        if (!answer.empty()) {
            cout << "✅ Ответ получен: " << answer.size() << " символов" << endl;
            return answer;
        }
        else {
            return "Не удалось получить ответ. Попробуйте задать вопрос иначе.";
        }

    }
    catch (const exception& e) {
        cerr << "❌ Исключение в generateAnswerWithRAG: " << e.what() << endl;
        return "Внутренняя ошибка. Попробуйте позже.";
    }
}

// Проверка API
void checkAPI() {
    cout << "🔍 Проверка доступности Yandex Cloud API..." << endl;

    CURL* curl = curl_easy_init();
    string response;

    if (!curl) {
        cerr << "❌ Не удалось инициализировать CURL для проверки API" << endl;
        yandexApiAvailable = false;
        return;
    }

    json testRequest = {
        {"modelUri", "gpt://" + FOLDER_ID + "/yandexgpt-lite"},
        {"completionOptions", {
            {"stream", false},
            {"temperature", 0.1},
            {"maxTokens", "10"}
        }},
        {"messages", {
            {{"role", "user"}, {"text", "Привет"}}
        }}
    };

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    string authHeader = "Authorization: Api-Key " + API_KEY;
    headers = curl_slist_append(headers, authHeader.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, YANDEX_GPT_URL.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    string requestBody = testRequest.dump(-1, ' ', false, json::error_handler_t::replace);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        try {
            json jsonResponse = json::parse(response);
            if (jsonResponse.contains("result")) {
                yandexApiAvailable = true;
                cout << "✅ Yandex Cloud API доступен" << endl;
            }
            else if (jsonResponse.contains("error")) {
                cerr << "❌ API вернул ошибку: " << jsonResponse["error"]["message"] << endl;
                yandexApiAvailable = false;
            }
        }
        catch (...) {
            cerr << "❌ Неверный ответ от API" << endl;
            yandexApiAvailable = false;
        }
    }
    else {
        cerr << "❌ Ошибка подключения к API: " << curl_easy_strerror(res) << endl;
        yandexApiAvailable = false;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}


// ========== MAIN ==========
int main() {
    // Устанавливаем кодировку консоли
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // Инициализируем CURL
    curl_global_init(CURL_GLOBAL_ALL);

    cout << "========================================" << endl;
    cout << "🤖 Telegram Bot with Advanced RAG" << endl;
    cout << "📚 Книга: Путь наименьшего сопротивления" << endl;
    cout << "👤 Автор: Роберт Фритц" << endl;
    cout << "========================================" << endl;

    // Инициализация RAG
    if (!initRAGSystem()) {
        cerr << "❌ Критическая ошибка: Не удалось инициализировать RAG систему" << endl;
        cerr << "⚠️ Проверьте файлы book.txt и author.txt" << endl;

        curl_global_cleanup();
        return 1;
    }

    // Проверка API
    checkAPI();

    if (!yandexApiAvailable) {
        cout << "⚠️ Предупреждение: Yandex Cloud API недоступен" << endl;
        cout << "🤖 Бот будет работать в ограниченном режиме" << endl;
    }

    // Инициализируем бота
    Bot bot(BOT_TOKEN);

    bot.getEvents().onCommand("start", [&bot](Message::Ptr message) {
        string welcome = "🤖 Бот с RAG технологией\n\n";
        welcome += "📚 Отвечаю на вопросы по книге:\n";
        welcome += "«Путь наименьшего сопротивления»\n\n";
        welcome += "👤 Автор: Роберт Фритц\n\n";

        {
            lock_guard<mutex> dataLock(dataMutex);
            int withEmbeddings = 0;
            for (const auto& [chunk, embedding] : bookChunks) {
                if (!embedding.empty()) withEmbeddings++;
            }

            welcome += "💾 Проиндексировано: " + to_string(bookChunks.size()) + " фрагментов\n";
            if (withEmbeddings > 0) {
                welcome += "🔍 С эмбеддингами: " + to_string(withEmbeddings) + " фрагментов\n";
            }
        }

        welcome += "\n⚡ Технология: RAG (Retrieval-Augmented Generation)\n\n";
        welcome += "💡 Просто задавайте вопросы о книге!\n\n";
        welcome += "🔍 Примеры вопросов:\n";
        welcome += "• Что такое структурное напряжение?\n";
        welcome += "• Как создать желаемый результат?\n";
        welcome += "• В чем суть пути наименьшего сопротивления?";

        bot.getApi().sendMessage(message->chat->id, welcome);
        });

    bot.getEvents().onCommand("status", [&bot](Message::Ptr message) {
        string status = "📊 Статус системы\n\n";

        {
            lock_guard<mutex> dataLock(dataMutex);
            status += "📚 Фрагментов книги: " + to_string(bookChunks.size()) + "\n";

            int withEmbeddings = 0;
            for (const auto& [chunk, embedding] : bookChunks) {
                if (!embedding.empty()) withEmbeddings++;
            }
            status += "🔍 С эмбеддингами: " + to_string(withEmbeddings) + "\n";
        }

        status += "\n⚡ Yandex Cloud API: ";
        status += yandexApiAvailable ? "✅ Доступен" : "❌ Недоступен";
        status += "\n\n🤖 Бот работает " + string(yandexApiAvailable ? "в полном режиме" : "в fallback режиме");

        bot.getApi().sendMessage(message->chat->id, status);
        });


    bot.getEvents().onAnyMessage([&bot](Message::Ptr message) {
        if (message->text.empty() || message->text[0] == '/') return;

        try {
            // Пытаемся отправить действие "печатает"
            bot.getApi().sendChatAction(message->chat->id, "typing");
        }
        catch (const exception& e) {
            string errorMsg = e.what();
            if (errorMsg.find("bot was blocked") != string::npos ||
                errorMsg.find("Forbidden") != string::npos) {
                cerr << "⚠️ Пользователь " << message->chat->id << " заблокировал бота. Пропускаем." << endl;
                return;
            }
            cerr << "❌ Ошибка sendChatAction: " << errorMsg << endl;
        }

        string answer = generateAnswerWithRAG(message->text);

        try {
            bot.getApi().sendMessage(message->chat->id, answer);
        }
        catch (const exception& e) {
            string errorMsg = e.what();

            // Проверяем, заблокирован ли бот
            if (errorMsg.find("bot was blocked") != string::npos ||
                errorMsg.find("Forbidden") != string::npos) {
                cerr << "⚠️ Бот заблокирован пользователем " << message->chat->id
                    << ". Пропускаем сообщение." << endl;
                return;
            }

            // Другие ошибки
            cerr << "❌ Ошибка отправки сообщения: " << errorMsg << endl;

            // Попытка отправить укороченное сообщение
            if (answer.length() > 4000) {
                answer = answer.substr(0, 4000) + "...\n\n[сообщение обрезано]";
                try {
                    bot.getApi().sendMessage(message->chat->id, answer);
                }
                catch (const exception& e2) {
                    cerr << "❌ Ошибка отправки обрезанного сообщения: " << e2.what() << endl;
                }
            }
        }
        });

    try {
        cout << "\n🚀 Бот запущен!" << endl;
        cout << "📞 Имя бота: @" << bot.getApi().getMe()->username << endl;
        cout << "========================================" << endl;


        TgLongPoll longPoll(bot);
        while (true) {
            try {
                longPoll.start();
            }
            catch (const exception& e) {
                string errorMsg = e.what();

                // Если бот заблокирован - просто игнорируем и продолжаем
                if (errorMsg.find("bot was blocked") != string::npos ||
                    errorMsg.find("Forbidden") != string::npos) {
                    // Слишком частое логирование - убираем или сокращаем
                    static int errorCount = 0;
                    if (errorCount++ % 10 == 0) { // Логируем только каждое 10-е сообщение
                        cerr << "⚠️ Получено обновление от заблокировавшего пользователя. Пропускаем..." << endl;
                    }
                    continue;
                }

                cerr << "❌ Другая ошибка в longPoll: " << errorMsg << endl;
                cout << "🔄 Перезапуск через 5 секунд..." << endl;
                mySleep(5000);
            }
        }
    }
    catch (const exception& e) {
        cerr << "❌ Критическая ошибка бота: " << e.what() << endl;
        cerr << "🔄 Попробуйте перезапустить программу" << endl;
    }

    curl_global_cleanup();
    return 0;
   
}