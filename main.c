#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_LINE 1024
#define QUEUE_CAPACITY 1024
#define MAX_DEVICE_NAME 64
#define SENSOR_COUNT 6

// índice dos sensores no CSV (0=id,1=device,2=contagem,3=data,...)
const int sensor_field_idx[SENSOR_COUNT] = {4, 5, 6, 7, 8, 9};
const char *sensor_names[SENSOR_COUNT] = {
    "Temperatura", "Umidade", "Luminosidade", "Ruido", "eco2", "etvoc"};

// fila de strings (linhas CSV)
char *queue[QUEUE_CAPACITY];
int q_head = 0, q_tail = 0, q_count = 0;
bool done_reading = false;

pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t q_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t q_not_full = PTHREAD_COND_INITIALIZER;

// estrutura de agregado
typedef struct
{
    char id_hash[100]; // year_month + sensor
    char device[MAX_DEVICE_NAME];
    int year_month; // ex: 202403
    int sensor;     // 0..5
    float min, max, sum;
    int count;
} Resultado;

// estrutura de resultado para impressão
typedef struct {
    char device[MAX_DEVICE_NAME];
    char ano_mes[8];    // “YYYY-MM”
    char sensor[SENSOR_COUNT];
    float max, avg, min;
} Entry;

Resultado *results_array = NULL;
size_t result_size = 0, result_cap = 0;
pthread_mutex_t resultado_mutex = PTHREAD_MUTEX_INITIALIZER;

// comparador para qsort: primeiro por device, depois sensor, depois ano_mes
static int cmp_entries(const void *a, const void *b) {
    const Entry *e1 = a, *e2 = b;
    int r = strcmp(e1->device, e2->device);
    if (r) return r;
    r = strcmp(e1->sensor, e2->sensor);
    if (r) return r;
    return strcmp(e1->ano_mes, e2->ano_mes);
}

void print_results() {
    FILE *f = fopen("./devices_mqtt_data/resultados.csv", "r");
    if (!f) {
        perror("fopen");
        return;
    }

    char line[MAX_LINE];
    // pula cabeçalho
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return;
    }

    Entry *arr = NULL;
    size_t count = 0, cap = 0;

    // lê cada linha do CSV
    while (fgets(line, sizeof(line), f)) {
        char *tok;
        char dev[MAX_DEVICE_NAME], mes[8], sensor[SENSOR_COUNT];
        float vmax, vavg, vmin;

        // device
        tok = strtok(line, ";\n");
        if (!tok) continue;
        strncpy(dev, tok, MAX_DEVICE_NAME); dev[MAX_DEVICE_NAME-1] = '\0';

        // ano-mes
        tok = strtok(NULL, ";\n");
        if (!tok) continue;
        strncpy(mes, tok, sizeof(mes)); mes[sizeof(mes)-1] = '\0';

        // sensor
        tok = strtok(NULL, ";\n");
        if (!tok) continue;
        strncpy(sensor, tok, SENSOR_COUNT); sensor[SENSOR_COUNT-1] = '\0';

        // valores numéricos
        tok = strtok(NULL, ";\n"); if (!tok) continue; vmax = atof(tok);
        tok = strtok(NULL, ";\n"); if (!tok) continue; vavg = atof(tok);
        tok = strtok(NULL, ";\n"); if (!tok) continue; vmin = atof(tok);

        // expande array
        if (count == cap) {
            cap = cap ? cap * 2 : 128;
            arr = realloc(arr, cap * sizeof(*arr));
            if (!arr) {
                perror("realloc");
                fclose(f);
                return;
            }
        }
        // armazena entry
        Entry *e = &arr[count++];
        strcpy(e->device, dev);
        strcpy(e->ano_mes, mes);
        strcpy(e->sensor, sensor);
        e->max = vmax;  e->avg = vavg;  e->min = vmin;
    }
    fclose(f);

    // ordena por dispositivo → sensor → ano_mes
    qsort(arr, count, sizeof(*arr), cmp_entries);

    // imprime agrupado
    char current_dev[MAX_DEVICE_NAME] = "";
    for (size_t i = 0; i < count; i++) {
        Entry *e = &arr[i];
        if (strcmp(current_dev, e->device) != 0) {
            // novo dispositivo
            strcpy(current_dev, e->device);
            printf("\n=== Device: %s ===\n", current_dev);
        }
        printf("  %-12s [%s] → max: %.2f, avg: %.2f, min: %.2f\n",
               e->sensor, e->ano_mes, e->max, e->avg, e->min);
    }

    free(arr);
}

// enfileira uma linha (duplica string) na memória (região crítica)
void enqueue(char *line)
{
    pthread_mutex_lock(&queue_mutex);
    while (q_count == QUEUE_CAPACITY)
    {
        pthread_cond_wait(&q_not_full, &queue_mutex);
    }
    queue[q_tail] = line;
    q_tail = (q_tail + 1) % QUEUE_CAPACITY;
    q_count++;
    pthread_cond_signal(&q_not_empty);
    pthread_mutex_unlock(&queue_mutex);
}

// desenfileira ou retorna NULL se estiver tudo processado na memória (região crítica)
char *dequeue()
{
    pthread_mutex_lock(&queue_mutex);
    while (q_count == 0 && !done_reading)
    {
        pthread_cond_wait(&q_not_empty, &queue_mutex);
    }
    if (q_count == 0 && done_reading)
    {
        pthread_mutex_unlock(&queue_mutex);
        return NULL;
    }
    char *line = queue[q_head];
    q_head = (q_head + 1) % QUEUE_CAPACITY;
    q_count--;
    pthread_cond_signal(&q_not_full);
    pthread_mutex_unlock(&queue_mutex);
    return line;
}

// atualiza (ou cria) um agregado para device+year_month+sensor
void update_resultado(const char *device, int ym, int sensor, float value)
{
    pthread_mutex_lock(&resultado_mutex);

    // 1) procura existing entry
    for (size_t i = 0; i < result_size; i++)
    {
        Resultado *e = &results_array[i];
        if (e->year_month == ym && e->sensor == sensor && strcmp(e->device, device) == 0)
        {
            // encontrado: atualiza min/max/soma/contagem
            if (value < e->min)
                e->min = value;
            if (value > e->max)
                e->max = value;
            e->sum += value;
            e->count++;
            pthread_mutex_unlock(&resultado_mutex);
            return;
        }
    }

    // 2) não encontrou → cria novo entry dentro do lock
    if (result_size == result_cap)
    {
        result_cap = result_cap ? result_cap * 2 : 128;
        Resultado *tmp = realloc(results_array, result_cap * sizeof(*results_array));
        if (!tmp)
        {
            perror("realloc");
            exit(EXIT_FAILURE);
        }
        results_array = tmp;
    }

    // inicializa o novo elemento
    Resultado *e = &results_array[result_size++];
    strncpy(e->device, device, sizeof(e->device) - 1);
    e->device[sizeof(e->device) - 1] = '\0';
    e->year_month = ym;
    e->sensor = sensor;
    e->min = e->max = e->sum = value;
    e->count = 1;

    pthread_mutex_unlock(&resultado_mutex);
}

// parse da linha e atualização de resultados
void process_line(char *line)
{

    char *fields[12];

    int i = 0;

    char *token = strtok(line, "|\n"); // primeiro token

    while (token != NULL && i < 12)
    {
        fields[i++] = token;
        token = strtok(NULL, "|\n"); // próximos tokens da linha
    }
    // printf("i=%d\n", i);
    // printf("chegou a linha: %s\n", fields[0]);

    while (i < 12 && (fields[i] = strtok(NULL, ";\n")))
    {
        i++;
    }
    if (i < 10)
    {
        return; // linha incompleta
    }

    // parse date
    int year, month, day;

    if (sscanf(fields[3], "%d-%d-%d", &year, &month, &day) != 3)
    {
        return;
    }

    if (year < 2024 || (year == 2024 && month < 3))
    {
        //printf("ignorado: %s\n", fields[3]);
        return;
    }

    char device[MAX_DEVICE_NAME];
    // printf("processando %d", device);

    // copia nome do dispositivo
    strncpy(device, fields[1], MAX_DEVICE_NAME - 1);
    device[MAX_DEVICE_NAME - 1] = '\0';

    int ano_mes = year * 100 + month;

    // para cada sensor
    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        //printf("processando %s %d-%02d %s %s\n", device, year, month, sensor_names[i], fields[sensor_field_idx[i]]);
        float floatValue = atof(fields[sensor_field_idx[i]]);
        update_resultado(device, ano_mes, i, floatValue);
    }
}

// thread produtora: lê o CSV e enfileira
void *reader_thread(void *arg)
{
    FILE *devicesFile = fopen("./devices_mqtt_data/devices.csv", "r");

    if (!devicesFile)
    {
        perror("fopen");
        exit(1);
    }

    char buf[MAX_LINE];

    // pula cabeçalho
    if (!fgets(buf, sizeof(buf), devicesFile))
    {
        fclose(devicesFile);
        done_reading = true;
        return NULL;
    }

    // lê todas as linhas
    while (fgets(buf, sizeof(buf), devicesFile))
    {
        char *copy = strdup(buf);
        // printf("Enfileirando: %s\n", copy);
        enqueue(copy);
    }

    fclose(devicesFile);

    // sinaliza fim
    pthread_mutex_lock(&queue_mutex);
    done_reading = true;
    pthread_cond_broadcast(&q_not_empty);
    pthread_mutex_unlock(&queue_mutex);
    return NULL;
}

// threads consumidoras: processam até a fila esvaziar e done_reading=true
void *worker_thread(void *arg) {
    int worker_id = (int)(intptr_t)arg;
    while (true) {
        char *line = dequeue();
        if (!line) break;
        //printf("Worker %d — desenfileirou: %s\n", worker_id, line);
        process_line(line);
        free(line);
    }
    return NULL;
}

// grava CSV final com todos os agregados
void write_results()
{
    FILE *f = fopen("./devices_mqtt_data/resultados.csv", "w");
    if (!f)
    {
        perror("fopen");
        return;
    }

    // cabeçalho
    fprintf(f, "device;ano-mes;sensor;valor_maximo;valor_medio;valor_minimo\n");

    for (size_t i = 0; i < result_size; i++)
    {
        Resultado *e = &results_array[i];

        float avg = e->sum / e->count;
        // formata ano-mes como YYYY-MM
        int y = e->year_month / 100;
        int m = e->year_month % 100;
        fprintf(f, "%s;%04d-%02d;%s;%.2f;%.2f;%.2f\n",
                e->device, y, m,
                sensor_names[e->sensor],
                e->max, avg, e->min);
    }

    fclose(f);
}

int main()
{
    // descobre núcleos
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    printf("Núcleos disponíveis: %ld\n", cores);

    int n_workers = (cores >= 2 ? cores - 1 : 1);
    printf("Criando %d threads de trabalho\n", n_workers);

    printf("Processando...\n");
    pthread_t reader;
    pthread_create(&reader, NULL, reader_thread, NULL); // thread produtora

    pthread_t workers[n_workers];
    for (int i = 0; i < n_workers; i++) {
        pthread_create(&workers[i], NULL, worker_thread, (void*)(intptr_t)i);
    } // threads consumidoras

    // // aguarda fim da leitura e processamento
    pthread_join(reader, NULL);
    for (int i = 0; i < n_workers; i++)
        pthread_join(workers[i], NULL);

    write_results();

    print_results();

    free(results_array);
    return 0;
}
