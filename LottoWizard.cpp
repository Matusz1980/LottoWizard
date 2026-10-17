#include <iostream>
#include <set>
#include <string>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <wx/wx.h>
#include <wx/stdpaths.h> // Wymagane dla ścieżek systemowych
#include <wx/filename.h>  // Pomaga w zarządzaniu ścieżkami plików
#include <wx/grid.h>
#include <array>
#include <algorithm>
#include <functional>

using json = nlohmann::json;

static constexpr int kNumberColumns = 42;
static constexpr int kTotalColumns = 2 + kNumberColumns; // ID, Data + 42 pól liczb

// Callback dla libcurl zbierający odpowiedź z API
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Konwertuje epoch (UTC) do czytelnego stringa UTC "YYYY-MM-DD HH:MM:SS"
static std::string EpochToString(std::time_t epoch) {
    std::tm buf_tm;
#if defined(_WIN32)
    gmtime_s(&buf_tm, &epoch);
#else
    gmtime_r(&epoch, &buf_tm);
#endif
    char buff[64];
    std::strftime(buff, sizeof(buff), "%Y-%m-%d %H:%M:%S", &buf_tm);
    return std::string(buff);
}

// Konwertuje epoch (UTC) do czasu polskiego uwzględniając DST i zwraca "YYYY-MM-DD HH:MM:SS"
static std::string EpochToPolandString(std::time_t epoch) {
    // Zmieniamy TZ procesu tymczasowo na Europe/Warsaw, żeby localtime_* zwracało czas polski
    // Zachowujemy poprzednie TZ i przywracamy po konwersji.
#if defined(_WIN32)
    char* prev = nullptr;
    size_t req = 0;
    _dupenv_s(&prev, &req, "TZ");
    std::string old = prev ? std::string(prev) : std::string();
    _putenv_s("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3");
    _tzset();
    std::tm buf_tm;
    localtime_s(&buf_tm, &epoch);
    if (prev) free(prev);
    if (old.empty()) _putenv_s("TZ", ""); else _putenv_s("TZ", old.c_str());
    _tzset();
#else
    const char* prev = std::getenv("TZ");
    std::string old = prev ? std::string(prev) : std::string();
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();
    std::tm buf_tm{};
    localtime_r(&epoch, &buf_tm);
    if (old.empty()) unsetenv("TZ"); else setenv("TZ", old.c_str(), 1);
    tzset();
#endif

    // Recompute localtime into buf_tm above. For portability let's compute again (windows/unix separated).
    std::tm final_tm{};
#if defined(_WIN32)
    // reuse localtime_s result computed earlier
    localtime_s(&final_tm, &epoch);
#else
    localtime_r(&epoch, &final_tm);
#endif

    char buff[64];
    std::strftime(buff, sizeof(buff), "%Y-%m-%d %H:%M:%S", &final_tm);
    return std::string(buff);
}

// Zwraca ciąg ISO8601 UTC typu "YYYY-MM-DDTHH:MM:SSZ" dla podanego epoch (UTC)
static std::string EpochToIsoUtcString(std::time_t epoch) {
    std::tm buf_tm;
#if defined(_WIN32)
    gmtime_s(&buf_tm, &epoch);
#else
    gmtime_r(&epoch, &buf_tm);
#endif
    char buff[64];
    std::strftime(buff, sizeof(buff), "%Y-%m-%dT%H:%M:%SZ", &buf_tm);
    return std::string(buff);
}

// Parsuje datę ISO8601 (np. "2026-05-26T22:00:00Z" lub "2026-05-26T22:00:00+02:00")
// i zwraca sekundę EPOCH (UTC). Zwraca 0 przy błędzie.
static std::time_t ParseDateToEpoch(const std::string& dateStr) {
    if (dateStr.empty()) return 0;

    std::string s = dateStr;
    size_t tpos = s.find('T');
    size_t offsetPos = std::string::npos;
    if (tpos != std::string::npos) {
        for (size_t i = tpos + 1; i < s.size(); ++i) {
            if (s[i] == '+' || s[i] == '-') { offsetPos = i; break; }
            if (s[i] == 'Z' || s[i] == 'z') { offsetPos = i; break; }
        }
    }

    std::string offsetStr;
    if (offsetPos != std::string::npos && offsetPos < s.size()) {
        if (s[offsetPos] == '+' || s[offsetPos] == '-') {
            offsetStr = s.substr(offsetPos);
            s = s.substr(0, offsetPos);
        } else {
            s = s.substr(0, offsetPos);
            offsetStr.clear();
        }
    } else {
        size_t lastColon = s.rfind(':');
        if (lastColon != std::string::npos && lastColon + 3 < s.size()) {
            s = s.substr(0, lastColon + 3);
        }
    }

    if (!s.empty() && (s.back() == 'Z' || s.back() == 'z')) s.pop_back();

    std::tm tm = {};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) {
        ss.clear();
        ss.str(s);
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (ss.fail()) return 0;
    }

#if defined(_WIN32)
    std::time_t epoch = _mkgmtime(&tm);
#else
    std::time_t epoch = timegm(&tm);
#endif
    if (epoch == (std::time_t)-1) return 0;

    if (!offsetStr.empty()) {
        char sign = offsetStr[0];
        int oh = 0, om = 0;
        if (offsetStr.size() >= 3) {
            std::string body = offsetStr.substr(1);
            size_t colon = body.find(':');
            if (colon != std::string::npos) {
                oh = std::stoi(body.substr(0, colon));
                om = std::stoi(body.substr(colon + 1));
            } else if (body.size() == 4) {
                oh = std::stoi(body.substr(0,2));
                om = std::stoi(body.substr(2,2));
            } else {
                oh = std::stoi(body);
            }
        }
        int offsetSeconds = oh * 3600 + om * 60;
        if (sign == '+') epoch -= offsetSeconds; else epoch += offsetSeconds;
    }

    return epoch;
}

// Pomocnicze: ustawia TZ procesu na "Europe/Warsaw", zwraca poprzednią wartość TZ (może być empty).
static std::string SetPolandTZ() {
#if defined(_WIN32)
    char* prev = nullptr;
    size_t required = 0;
    _dupenv_s(&prev, &required, "TZ");
    std::string old = prev ? std::string(prev) : std::string();
    _putenv_s("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3");
    _tzset();
    if (prev) free(prev);
    return old;
#else
    const char* prev = std::getenv("TZ");
    std::string old = prev ? std::string(prev) : std::string();
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();
    return old;
#endif
}

// Przywraca poprzednią wartość TZ procesu (jeśli empty -> usuwa TZ)
static void RestoreTZ(const std::string& old) {
#if defined(_WIN32)
    if (old.empty()) _putenv_s("TZ", ""); else _putenv_s("TZ", old.c_str());
    _tzset();
#else
    if (old.empty()) unsetenv("TZ"); else setenv("TZ", old.c_str(), 1);
    tzset();
#endif
}

// Zwraca ISO8601 UTC instant odpowiadający godzinie 22:00 czasu polskiego
// dla podanej daty (year, month, day).
static std::string PolandDateAt22ToUtcIso(int year, int month, int day) {
    std::string oldTz = SetPolandTZ();

    std::tm tm_local = {};
    tm_local.tm_year = year - 1900;
    tm_local.tm_mon  = month - 1;
    tm_local.tm_mday = day;
    tm_local.tm_hour = 22;
    tm_local.tm_min  = 0;
    tm_local.tm_sec  = 0;
    tm_local.tm_isdst = -1; // pozwól systemowi ustalić DST

    std::time_t epoch22 = mktime(&tm_local);

    RestoreTZ(oldTz);

    if (epoch22 == (std::time_t)(-1)) return std::string();
    return EpochToIsoUtcString(epoch22);
}

// FetchLottoDataFromApi: przyjmuje drawDate w ISO UTC i zwraca JSON string
static std::string FetchLottoDataFromApi(const std::string& drawDateUtcIso) {
    const std::string baseUrl = "https://developers.lotto.pl/api/open/v1/lotteries/draw-results/by-date-per-game";
    std::ostringstream q;
    q << "?gameType=MiniLotto&drawDate=" << drawDateUtcIso << "&index=1&size=5&sort=drawSystemId&order=ASC";
    std::string fullUrl = baseUrl + q.str();

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string readBuffer;
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "secret: HERE_PASTE_YOUR_API_KEY_FOR_LOTTO_REST_SERVICES");
    curl_easy_setopt(curl, CURLOPT_URL, fullUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    int retries = 0;
    while (retries < 3) {
        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res == CURLE_OK) {
            if (http_code == 200) break;
            if (http_code == 429) {
                std::this_thread::sleep_for(std::chrono::seconds(60));
                ++retries;
                continue;
            }
            readBuffer.clear();
            break;
        } else {
            readBuffer.clear();
            break;
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return readBuffer;
}

// Sprawdza, czy rysowanie (id) jest już w bazie
static bool DrawExists(sqlite3* db, int drawId) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(1) FROM mini_lotto WHERE id_losowania = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_bind_int(stmt, 1, drawId);
    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) exists = sqlite3_column_int(stmt, 0) > 0;
    sqlite3_finalize(stmt);
    return exists;
}

// Wstawia wynik losowania do bazy (data_losowania jako INTEGER epoch)
static bool InsertDraw(sqlite3* db, int id, std::time_t epochUtc, const std::vector<int>& numbers) {
    const char* sql = "INSERT OR REPLACE INTO mini_lotto (id_losowania, data_losowania, num1, num2, num3, num4, num5) VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(epochUtc));
    for (int i = 0; i < 5; ++i) sqlite3_bind_int(stmt, 3 + i, numbers[i]);

    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_DONE) ok = true;
    sqlite3_finalize(stmt);
    return ok;
}

// Funkcja pobierająca dane z API i zapisująca do SQLite (pozostawiona bez zmian)
void FetchAndStoreMiniLotto(sqlite3* db) {
    using namespace std::chrono;

    // Pomocnicza funkcja: zwraca maksymalną wartość data_losowania z bazy (epoch seconds) lub 0 jeśli brak
    auto GetMaxDataLosowania = [](sqlite3* db) -> std::time_t {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT MAX(data_losowania) FROM mini_lotto;";
        std::time_t result = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
                    result = static_cast<std::time_t>(sqlite3_column_int64(stmt, 0));
                }
            }
        }
        sqlite3_finalize(stmt);
        return result;
    };

    // Obecny czas (epoch)
    system_clock::time_point now_tp = system_clock::now();
    std::time_t epoch_now = system_clock::to_time_t(now_tp);

    // Domyślny start: 1 miesiące wstecz (aproksymacja 30 dni)
    std::time_t default_start;
    {
        system_clock::time_point now = system_clock::now();
        system_clock::time_point start_default = now - hours(24 * 30);
        default_start = system_clock::to_time_t(start_default);
        // zaokrąglij do początku dnia UTC
#if defined(_WIN32)
        std::tm tm_start;
        gmtime_s(&tm_start, &default_start);
#else
        std::tm tm_start;
        gmtime_r(&default_start, &tm_start);
#endif
        tm_start.tm_hour = 0; tm_start.tm_min = 0; tm_start.tm_sec = 0;
#if defined(_WIN32)
        default_start = _mkgmtime(&tm_start);
#else
        default_start = timegm(&tm_start);
#endif
    }

    // Sprawdź co jest w bazie i ustaw punkt startu
    std::time_t max_epoch = GetMaxDataLosowania(db);
    std::time_t start_epoch = 0;
    if (max_epoch > 0) {
        // Zaczynamy od następnego dnia po ostatnim zapisie
        start_epoch = max_epoch + 24 * 60 * 60;
        if (start_epoch > epoch_now) {
            // Najnowszy wpis jest z dzisiaj (lub z przyszłości) -> nic do zrobienia
            wxLogDebug("Brak nowych dni do synchronizacji (ostatnie data_losowania = %lld)", static_cast<long long>(max_epoch));
            return;
        }
    } else {
        // Brak danych w bazie -> start 3 miesiące wstecz
        start_epoch = default_start;
    }

    // Zaokrąglij start_epoch do początku dnia UTC aby iteracja była deterministyczna
#if defined(_WIN32)
    std::tm tm_s;
    gmtime_s(&tm_s, &start_epoch);
#else
    std::tm tm_s;
    gmtime_r(&start_epoch, &tm_s);
#endif
    tm_s.tm_hour = 0; tm_s.tm_min = 0; tm_s.tm_sec = 0;
#if defined(_WIN32)
    start_epoch = _mkgmtime(&tm_s);
#else
    start_epoch = timegm(&tm_s);
#endif

    // Iteruj dzień po dniu od start_epoch do teraz (włącznie)
    for (std::time_t day = start_epoch; day <= epoch_now; day += 24 * 60 * 60) {
        // Wyznacz datę kalendarzową w strefie polskiej dla tego dnia i ustaw 22:00 lokalnie
        std::string oldTz = SetPolandTZ();

        std::tm tmPoland = {};
#if defined(_WIN32)
        localtime_s(&tmPoland, &day);
#else
        localtime_r(&day, &tmPoland);
#endif
        int year = tmPoland.tm_year + 1900;
        int month = tmPoland.tm_mon + 1;
        int mday = tmPoland.tm_mday;

        RestoreTZ(oldTz);

        // drawDateParam ma wskazywać na 22:00 czasu polskiego (przekonwertowane na ISO UTC)
        std::string drawDateParam = PolandDateAt22ToUtcIso(year, month, mday);
        if (drawDateParam.empty()) continue;

        std::string response = FetchLottoDataFromApi(drawDateParam);
        if (response.empty()) continue;

        try {
            auto j = json::parse(response);

            if (j.contains("items") && j["items"].is_array()) {
                for (const auto& draw : j["items"]) {
                    if (!draw.contains("drawSystemId")) continue;
                    int id = draw["drawSystemId"].get<int>();

                    if (DrawExists(db, id)) continue;

                    // Parsowanie daty losowania (jeśli dostępna)
                    std::time_t epoch = 0;
                    if (draw.contains("drawDate") && draw["drawDate"].is_string()) {
                        epoch = ParseDateToEpoch(draw["drawDate"].get<std::string>());
                    }
                    if (epoch == 0) {
                        // fallback: użyj epoch odpowiadającego 22:00 UTC, sparsowanego z drawDateParam
                        std::tm tm = {};
                        std::string s = drawDateParam;
                        size_t lastColon = s.rfind(':');
                        if (lastColon != std::string::npos && lastColon + 3 <= s.size()) {
                            s = s.substr(0, lastColon + 3);
                        }
                        std::istringstream ss(s);
                        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
#if defined(_WIN32)
                        epoch = _mkgmtime(&tm);
#else
                        epoch = timegm(&tm);
#endif
                    }

                    // Parsowanie liczb wyników
                    std::vector<int> numbers;
                    try {
                        if (draw.contains("results") && draw["results"].is_array() && !draw["results"].empty()) {
                            const auto& r0 = draw["results"][0];
                            if (r0.contains("resultsJson") && r0["resultsJson"].is_array()) {
                                for (const auto& n : r0["resultsJson"]) {
                                    numbers.push_back(n.get<int>());
                                }
                            }
                        }
                    } catch (...) { numbers.clear(); }

                    if (numbers.size() >= 5) {
                        numbers.resize(5);
                        InsertDraw(db, id, epoch, numbers);
                    }
                }
            }
        } catch (const std::exception&) {
            // ignoruj błędy parsowania pojedynczego dnia i kontynuuj
        }

        // Krótka przerwa, żeby nie zalewać API
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    wxLogDebug("Synchronizacja zakończona.");
}

// --- Nowa klasa tabeli oparta na wxGridTableBase ------------------------------
class LottoGridTable : public wxGridTableBase {
public:
    struct Row {
        int id; // dodatkowe pole ID (przydatne do wyświetlenia)
        wxDateTime drawDate; // wymagane pole
        std::array<int, 42> numbers; // wymagane pole: 42 elementy
        Row() : id(0), drawDate(), numbers{} { numbers.fill(0); }
    };

    LottoGridTable(sqlite3* database)
        : db(database)
    {
        LoadFromDB();
    }

    // Załaduj dane z bazy do vector<Row>
    void LoadFromDB() {
        rows.clear();
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT id_losowania, data_losowania, num1, num2, num3, num4, num5 FROM mini_lotto ORDER BY id_losowania DESC;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                Row r;
                r.id = sqlite3_column_int(stmt, 0);
                sqlite3_int64 epoch_val = sqlite3_column_int64(stmt, 1);
                // epoch stored in UTC seconds -> wxDateTime uses time_t
                r.drawDate.Set(static_cast<time_t>(epoch_val));
                // inicjalizuj tablicę zerami
                r.numbers.fill(0);
                // pobierz dostępne numery (num1..num5), reszta pozostaje 0
                for (int i = 0; i < 5; ++i) {
                    if (sqlite3_column_type(stmt, 2 + i) != SQLITE_NULL) {
                        int number = sqlite3_column_int(stmt, 2 + i);
                        r.numbers[number - 1] = number;
                    }
                }
                rows.push_back(std::move(r));
            }
        }
        sqlite3_finalize(stmt);
    }

    // Sortowanie po kolumnie: col - indeks kolumny gridu (0..)
    void SortByColumn(int col, bool ascending) {
        auto cmp = [this, col, ascending](const Row& a, const Row& b) -> bool {
            if (col == 0) { // ID
                return ascending ? (a.id < b.id) : (a.id > b.id);
            }
            else if (col == 1) { // Data
                time_t ta = a.drawDate.GetTicks();
                time_t tb = b.drawDate.GetTicks();
                return ascending ? (ta < tb) : (ta > tb);
            }
            else if (col >= 2 && col < GetNumberCols()) {
                int idx = col - 2;
                int va = a.numbers[idx];
                int vb = b.numbers[idx];
                // wartości 0 traktujemy jako "brak" - traktujemy je jako mniejsze niż niezerowe
                if (va == vb) return ascending ? (a.id < b.id) : (a.id > b.id); // fallback stable by id
                if (va == 0) return !ascending; // push zeros to end when ascending
                if (vb == 0) return ascending;
                return ascending ? (va < vb) : (va > vb);
            }
            return false;
        };

        std::stable_sort(rows.begin(), rows.end(), cmp);
    }

    // Udostępnione API
    int GetNumberRows() override { return static_cast<int>(rows.size()); }
    int GetNumberCols() override { return 2 + 42; } // ID, Data + 42 kolumn liczb
    bool IsEmptyCell(int row, int col) override {
        if (row < 0 || row >= GetNumberRows() || col < 0 || col >= GetNumberCols()) return true;
        if (col >= 2) {
            int val = rows[row].numbers[col - 2];
            return val == 0;
        }
        // ID i Data zawsze mają wartość (ID może być 0)
        return false;
    }

    wxString GetValue(int row, int col) override {
        if (row < 0 || row >= GetNumberRows() || col < 0 || col >= GetNumberCols()) return {};
        const Row& r = rows[row];
        if (col == 0) {
            return wxString::Format("%d", r.id);
        }
        else if (col == 1) {
            // Wyświetl czas w strefie Polski (format "YYYY-MM-DD HH:MM:SS")
            wxDateTime dt = r.drawDate;
            return dt.FormatISOCombined(' ');
        }
        else {
            int val = r.numbers[col - 2];
            if (val == 0) return {};
            return wxString::Format("%d", val);
        }
    }

    void SetValue(int row, int col, const wxString& value) override {
        // Tabela jest tylko do odczytu w UI - nie wspieramy SetValue.
        (void)row; (void)col; (void)value;
    }

    wxString GetColLabelValue(int col) override {
        wxString base;
        if (col == 0) base = "ID Losowania";
        else if (col == 1) base = "Data";
        else if (col >= 2 && col < GetNumberCols()) base = wxString::Format("%d", col - 1);
        else base = "";

        if (col == sortCol) {
            // używamy ASCII caret/vee aby uniknąć problemów z glifami w fontach
            base += sortAscending ? wxString::FromUTF8("\x20\xE2\x86\x91") : wxString::FromUTF8("\x20\xE2\x86\x93");
        }
        return base;
    }

    wxString GetRowLabelValue(int row) override {
        return wxString::Format("%d", row + 1);
    }

    // Nowa metoda: ustawia znacznik sortowania w nagłówku kolumny
    void SetSortMarker(int col, bool ascending) {
        sortCol = col;
        sortAscending = ascending;
    }

private:
    sqlite3* db;
    std::vector<Row> rows;

    // sort marker used for labels
    int sortCol = -1;
    bool sortAscending = true;
};

// --- Frame z gridem opartym na LottoGridTable --------------------------------
class LottoFrame : public wxFrame {
private:
    wxGrid* grid;
    sqlite3* db;
    LottoGridTable* table;
    int lastSortedCol;
    bool lastAscending;

public:
    LottoFrame(const wxString& title, sqlite3* database)
        : wxFrame(NULL, wxID_ANY, title, wxDefaultPosition, wxDefaultSize),
          db(database),
          grid(nullptr),
          table(nullptr),
          lastSortedCol(-1),
          lastAscending(true)
    {
        // Inicjalizacja gridu
        grid = new wxGrid(this, wxID_ANY);

        // Utworzenie tabeli i przypisanie do gridu (grid przejmuje własność tabeli)
        table = new LottoGridTable(db);
        grid->SetTable(table, true /*takeOwnership*/);

        // Wyłącz edycję komórek (tabela tylko do odczytu)
        grid->EnableEditing(false);

        // Ustawienie nagłówków
        grid->SetColLabelValue(0, "ID Losowania");
        grid->SetColLabelValue(1, "Data");
        for (int i = 0; i < kNumberColumns; ++i) {
            std::ostringstream hdr;
            hdr << (i + 1);
            grid->SetColLabelValue(2 + i, wxString::FromUTF8(hdr.str().c_str()));
        }

        // Pozostałe opcje
        grid->EnableDragColSize(true);
        grid->EnableDragGridSize(false);

        // Umieszczenie gridu w sizerze
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(grid, 1, wxEXPAND | wxALL, 0);
        SetSizer(sizer);
        Layout();

        // Minimalne szerokości kolumn
        grid->SetColMinimalWidth(0, 100);
        grid->SetColMinimalWidth(1, 140);
        for (int i = 2; i < kTotalColumns; ++i) grid->SetColMinimalWidth(i, 40);

        // Dopasuj szerokości na podstawie zawartości pierwszego renderu
        grid->AutoSizeColumns();
        for (int i = 0; i < 42; i++)
        {
            wxGridCellAttr* attr = new wxGridCellAttr();
            attr->SetAlignment(wxALIGN_CENTRE, wxALIGN_CENTRE);
            grid->SetColAttr(2 + i, attr);
        }
        // Wycentrowanie etykiet nagłówków kolumn
        grid->SetColLabelAlignment(wxALIGN_CENTRE, wxALIGN_CENTRE);

        // Ustaw font nagłówka kolumn tak, by zawierał symbole Unicode (Windows: "Segoe UI Symbol")
        wxFont hdrFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Segoe UI Symbol");
        if (wxWindow* colLabelWin = grid->GetGridColLabelWindow()) {
            colLabelWin->SetFont(hdrFont);
            colLabelWin->Refresh();
        }
        grid->Refresh();

        // Oblicz wymaganą szerokość okna dynamicznie po AutoSizeColumns()
        // Sumujemy szerokości wszystkich kolumn + etykieta wierszy
        int totalColWidth = grid->GetRowLabelSize(); // szerokość etykiet wierszy
        for (int i = 0; i < kTotalColumns; ++i)
            totalColWidth += grid->GetColSize(i);

        // Dodaj margines na ramkę okna + ewentualny pionowy scrollbar
        // wxSystemSettings::GetMetric zwraca wartości zależne od OS i aktywnego motywu
        int scrollbarWidth = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, this);
        if (scrollbarWidth <= 0) scrollbarWidth = 20; // bezpieczny fallback
        int frameExtra = GetSize().GetWidth() - GetClientSize().GetWidth(); // obramowanie/dekoracje
        int targetWidth = totalColWidth + scrollbarWidth + frameExtra;

        // Ustaw rozmiar: szerokość dopasowana, wysokość domyślna (600 px)
        SetSize(wxSize(targetWidth, 600));
        SetMinSize(wxSize(targetWidth, 400));
        Center();

        // Bind kliknięcia nagłówka kolumny - sortowanie
        grid->Bind(wxEVT_GRID_LABEL_LEFT_CLICK, &LottoFrame::OnLabelLeftClick, this);
    }

private:
    // Helper: usuwa istniejące markery sortowania z tekstu etykiety
    static wxString StripSortMarker(const wxString& s) {
        wxString r = s;
        // używamy explicite bajtów UTF-8 zamiast dosłownych znaków w pliku źródłowym
        r.Replace(wxString::FromUTF8("\x20\xE2\x86\x91"), ""); // " ↑"
        r.Replace(wxString::FromUTF8("\x20\xE2\x86\x93"), ""); // " ↓"
        r.Replace(wxString::FromUTF8("\x20\xE2\x96\xB2"), ""); // " ▲"
        r.Replace(wxString::FromUTF8("\x20\xE2\x96\xBC"), ""); // " ▼"
        return r;
    }

    // Deklaracja metody obsługi kliknięcia (definicja poza klasą)
    void OnLabelLeftClick(wxGridEvent& evt);
};

// Definicja metody poza klasą (poprawna)
void LottoFrame::OnLabelLeftClick(wxGridEvent& evt)
{
    int col = evt.GetCol();
    if (col < 0) { // kliknięto etykietę wiersza
        evt.Skip();
        return;
    }

    // Zapamiętaj poprzednią kolumnę przed aktualizacją
    int prevCol = lastSortedCol;

    // Określ kierunek sortowania
    bool ascending = true;
    if (col == prevCol) ascending = !lastAscending;

    // Uaktualnij stan sortowania
    lastSortedCol = col;
    lastAscending = ascending;

    // Usuń marker tylko z poprzedniej kolumny (jeśli istnieje i różni się od aktualnej)
    if (prevCol >= 0 && prevCol != col) {
        wxString prevLbl = grid->GetColLabelValue(prevCol);
        wxString cleanPrev = StripSortMarker(prevLbl);
        if (cleanPrev != prevLbl) grid->SetColLabelValue(prevCol, cleanPrev);
    }

    // Ustaw marker dla aktualnie klikniętej kolumny
    wxString base = StripSortMarker(grid->GetColLabelValue(col));
    wxString arrow = ascending
        ? wxString::FromUTF8("\x20\xE2\x86\x91")  // " ↑"
        : wxString::FromUTF8("\x20\xE2\x86\x93"); // " ↓"
    grid->SetColLabelValue(col, base + arrow);
    wxLogDebug(base + arrow);
    // Wymuś odświeżenie nagłówków/okna siatki
    grid->Refresh(); grid->Update();
    if (grid->GetGridWindow()) grid->GetGridWindow()->Refresh();

    // Wykonaj sortowanie w tabeli (jeśli to nasza tabela) i odśwież widok
    LottoGridTable* t = dynamic_cast<LottoGridTable*>(grid->GetTable());
    if (t) {
        t->SetSortMarker(col, ascending); // tabela zwróci teraz etykietę z markerm
        t->SortByColumn(col, ascending);
        // powiadom grid, że etykiety i dane się zmieniły
        grid->ForceRefresh();
        grid->Refresh();
        grid->Update();
    }

    // Debug: wypisz kolumnę (usuń gdy niepotrzebne)
    wxLogDebug("OnLabelLeftClick: col=%d ascending=%d prevCol=%d", col, ascending, prevCol);
    // Nie wywołujemy evt.Skip() - zatrzymujemy domyślne zachowanie
}

class LottoApp : public wxApp {
    sqlite3* db = nullptr;
public:
    virtual bool OnInit() {
        SetAppName("LottoWizard");
        wxString dataDir = wxStandardPaths::Get().GetUserDataDir();

        if (!wxFileName::DirExists(dataDir)) {
            wxFileName::Mkdir(dataDir, 0755, wxPATH_MKDIR_FULL);
        }

        wxString dbPath = dataDir + wxFileName::GetPathSeparator() + "lotto_data.db";
        wxLogDebug("Ścieżka do bazy danych: %s", dbPath);

        if (sqlite3_open(dbPath.utf8_str(), &db) != SQLITE_OK) {
            wxMessageBox("Nie można utworzyć lub otworzyć bazy danych w katalogu użytkownika!",
                "Błąd bazy danych", wxOK | wxICON_ERROR);
            return false;
        }

        const char* createTableSql =
            "CREATE TABLE IF NOT EXISTS mini_lotto ("
            "id_losowania INTEGER PRIMARY KEY, "
            "data_losowania INTEGER, "
            "num1 INTEGER, num2 INTEGER, num3 INTEGER, num4 INTEGER, num5 INTEGER);"

        ;

        sqlite3_exec(db, createTableSql, NULL, NULL, NULL);

        // Synchronizacja (może być długa) — użyj wątku jeśli potrzebujesz nieblokującego UI.
        FetchAndStoreMiniLotto(db);

        LottoFrame* frame = new LottoFrame("Wyniki Mini Lotto z Bazy SQLite", db);
        frame->Show(true);
        return true;
    }

    virtual int OnExit() {
        if (db) sqlite3_close(db);
        return wxApp::OnExit();
    }
};

wxIMPLEMENT_APP(LottoApp);
