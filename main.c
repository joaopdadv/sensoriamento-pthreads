#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>

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

pthread_mutex_t queue_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t q_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t q_not_full = PTHREAD_COND_INITIALIZER;

// estrutura de agregado
typedef struct
{
    char device[MAX_DEVICE_NAME];
    int year_month; // ex: 202403
    int sensor;     // 0..5
    float min, max, sum;
    int count;
} Resultado;

Resultado *results_array = NULL;
size_t agg_size = 0, agg_cap = 0;
pthread_mutex_t resultado_mutex = PTHREAD_MUTEX_INITIALIZER;

// enfileira uma linha (duplica string) na memória (região crítica)
void enqueue(char *line)
{
    pthread_mutex_lock(&queue_mtx);
    while (q_count == QUEUE_CAPACITY)
    {
        pthread_cond_wait(&q_not_full, &queue_mtx);
    }
    queue[q_tail] = line;
    q_tail = (q_tail + 1) % QUEUE_CAPACITY;
    q_count++;
    pthread_cond_signal(&q_not_empty);
    pthread_mutex_unlock(&queue_mtx);
}

// desenfileira ou retorna NULL se estiver tudo processado na memória (região crítica)
char *dequeue()
{
    pthread_mutex_lock(&queue_mtx);
    while (q_count == 0 && !done_reading)
    {
        pthread_cond_wait(&q_not_empty, &queue_mtx);
    }
    if (q_count == 0 && done_reading)
    {
        pthread_mutex_unlock(&queue_mtx);
        return NULL;
    }
    char *line = queue[q_head];
    q_head = (q_head + 1) % QUEUE_CAPACITY;
    q_count--;
    pthread_cond_signal(&q_not_full);
    pthread_mutex_unlock(&queue_mtx);
    return line;
}

// atualiza (ou cria) um agregado para device+year_month+sensor
void update_resultado(const char *device, int ym, int sensor, float value)
{
    pthread_mutex_lock(&resultado_mutex);

    // 1) procura existing entry
    for (size_t i = 0; i < agg_size; i++)
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
    if (agg_size == agg_cap)
    {
        agg_cap = agg_cap ? agg_cap * 2 : 128;
        Resultado *tmp = realloc(results_array, agg_cap * sizeof(*results_array));
        if (!tmp)
        {
            perror("realloc");
            exit(EXIT_FAILURE);
        }
        results_array = tmp;
    }

    // inicializa o novo elemento
    Resultado *e = &results_array[agg_size++];
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
    printf("i=%d\n", i);
    printf("processando: %s\n", fields[3]);

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
        return;
    }

    char device[MAX_DEVICE_NAME];
    // copia nome do dispositivo
    strncpy(device, fields[1], MAX_DEVICE_NAME - 1);
    device[MAX_DEVICE_NAME - 1] = '\0';

    int ano_mes = year * 100 + month;

    // para cada sensor
    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        printf("processando %s %d-%02d %s %s\n", device, year, month, sensor_names[i], fields[sensor_field_idx[i]]);
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
        enqueue(copy);
    }

    fclose(devicesFile);

    // sinaliza fim
    pthread_mutex_lock(&queue_mtx);
    done_reading = true;
    pthread_cond_broadcast(&q_not_empty);
    pthread_mutex_unlock(&queue_mtx);
    return NULL;
}

// threads consumidoras: processam até a fila esvaziar e done_reading=true
void *worker_thread(void *arg)
{
    while (true)
    {
        char *line = dequeue();
        if (!line)
            break;
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

    for (size_t i = 0; i < agg_size; i++)
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

    int n_workers = (cores > 2 ? cores - 2 : 1);
    printf("Criando %d threads de trabalho\n", n_workers);

    pthread_t reader;
    pthread_create(&reader, NULL, reader_thread, NULL); // thread produtora

    pthread_t workers[n_workers];
    for (int i = 0; i < n_workers; i++)
        pthread_create(&workers[i], NULL, worker_thread, NULL); // threads consumidoras

    // aguarda fim da leitura e processamento
    pthread_join(reader, NULL);
    for (int i = 0; i < n_workers; i++)
        pthread_join(workers[i], NULL);

    write_results();

    free(results_array);
    return 0;
}
