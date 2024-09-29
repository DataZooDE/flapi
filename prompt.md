# Task description to implement a API service alled flAPI (because it is based on DuckDB)

We want to create a service based software that automatically spins up read-only APIs for static datasets without requiring you to write a single line of code. It builds on top of DuckDB and heavily relies on its SQL engines and extension ecosystem to 
connect to other databases. It should use the C-API of DuckDB. As frontend framework I want to use the modern C++ framework Crow (https://crowcpp.org/). 
The configuration is done via YAML files. The SQL files are templated, we use https://github.com/pantor/inja as templating engine.

One usecase is to connect to connect to BigQuer (via https://github.com/hafenkran/duckdb-bigquery) or to SAP services via (https://github.com/datazoode/erpl/). The APIs should be RESTful, ideally we also support OData v4. We should also generate Swagger specs automatically.

## Core features

### Building Data API

#### Connecting to different data sources via the DuckDB ecosystem (e.g. BigQuery, SAP via ERPL, Parquet, Iceberg on S3/GCS/Azure Blob Storage, Postgres, MySQL, etc.)

#### Writing SQL Templates:
A lot of power of flAPI comes from the ability to write SQL templates. We use https://github.com/pantor/inja as templating engine. It provides Jinja2-like syntax.

These articles cover fundamental concepts, best practices, and advanced techniques to help you harness the full power of SQL templates in building robust and efficient data APIs. The following articles are included in this section:

##### Dynamic Parameters:

The dynamic parameter is a crucial feature that allows you to use request parameters in your SQL files, making your query results change according to your API request parameters. You can use `{{ context.params.parameter }}` to get the request parameter. Here's a basic SQL syntax example:

```sql
SELECT * FROM public.users
WHERE age >= {{ context.params.age }};
```

```yaml
# users.yaml
urlPath: /users
request:
  - fieldName: age
    fieldIn: query
```

With parameters set in SQL template, you can dynamically filter the results based on the age parameter provided in the querystring

```
curl -X GET "http://localhost:3000/api/users?age=18"
```

You can also use parameters in the URL path. For example, if you want to get a specific user by its ID, you can use the following SQL template:

```sql
SELECT * FROM public.users
WHERE id = {{ context.params.id }};
```

and define the request fields in the configuration file:

```yaml
# users.yaml
urlPath: /users/:id
request:
  - fieldName: id
    fieldIn: path
```

Now, you can send a request with the id parameter in the URL path.

```
curl -X GET "http://localhost:3000/api/users/1"
```

You can also use parameters in the request header with the following configuration.

```yaml
# users.yaml
urlPath: /users/:id
request:
  - fieldName: id
    fieldIn: header
```

Now, you can send a request with the id parameter in the header.

```
curl -X GET "http://localhost:3000/api/users" -H "id: 1"
```

##### Access HTTP Request Properties
You can effortlessly access data from the HTTP request or custom headers as query conditions using the {{ context.req.xxx }} syntax in your SQL file.

Here's an example:

```sql
SELECT
  *
FROM
  "artists"
WHERE
  {{ context.req.method }} = 'GET'
AND
  {{ context.req.header.customParameters }} = 'custom parameters'
```

In this example, the query checks if the request method is 'GET' and if the custom header 'customParameters' has the expected value. You can use this approach to access various request properties and integrate them into your query conditions.


#### Caching Datasets:
The caching layer feature allows you to leverage caching to improve the performance and efficiency of your API queries. With the {% cache %} tag, you can query results directly from the cache layer storage, reducing the need for repeated queries to the data source. Let's explore how to use this feature effectively.

To provide efficient caching functionality, flAPI utilizes DuckDB as the underlying storage engine. This ensures high-performance caching and improves the overall query execution speed. The cache is configured in the `flapi.yaml` file.

```yaml
cache:
  schema: flapi
```

To utilize the caching layer, you can enclose your SQL query inside the {% cache %} and {% endcache %} tags. For example:

```sql
-- dept.sql
{% cache "cache_key" %}
SELECT * FROM cache_departments
WHERE "name" = {{ context.params.name }};
{% endcache %}
```

This tag enables the query to fetch the result from the cache layer storage. You can define which queries are preloaded into the cache layer by specifying cache settings in the configuration file. Here's an example configuration:

```yaml
# dept.yaml
urlPath: /departments
profile: bigquery-lakehouse
cache:
  - cacheTableName: 'cache_departments' # The name of the table in the cache layer storage
    sql: 'CREATE TABLE {{ cache_table_name }} AS  SELECT * FROM {{ source_schema }}.departments' # The SQL query to preload into the cache layer, this means typically to copy the source table into the cache layer.
```

Also, you can add the refresh interval configuration in the yaml file in the cache section using the refreshTime keyword.

```yaml
# dept.yaml
cache:
  - cacheTableName: 'cache_departments' # The name of the table in the cache layer storage
    sql: 'CREATE TABLE {{ cache_table_name }} AS  SELECT * FROM {{ source_schema }}.departments' 
    refreshTime: 1h # The refresh interval for the cache layer
```
The time format used in refreshTime should be compliant with the Javascript ms package.

Another possibility is to enable the refresh endpoint for the urlPath. This means that if a client sends a HTTP DELETE request to the urlPath, the cache will be deleted. To enable the refresh endpoint, set the refresh property to true in the cache section.

```yaml
# dept.yaml
cache:
  - cacheTableName: 'cache_departments' # The name of the table in the cache layer storage
    sql: 'CREATE TABLE {{ cache_table_name }} AS  SELECT * FROM {{ source_schema }}.departments'
    refreshEndpoint: true # Enable the refresh endpoint for the urlPath
```

#### Handling Errors: 
flAPI support error handling. To define an exception in your SQL templates, use the {% error "ERROR_CODE" %} syntax. When the template encounters an error during execution, the request processing halts further execution and sends an error code to the client, rather than returning query results.

Consider the following example, where a check is performed before executing the main query:

```sql
{% req user %}
SELECT COUNT(*) AS count FROM customers WHERE name = {{ context.params.name }}
{% endreq %}

{% if user.value()[0].count == 0 %}
  {% error "CUSTOMER_NOT_FOUND" %}
{% endif %}

SELECT * FROM customers
WHERE name = {{ context.params.name }}
LIMIT 1
```

In this case, if clients send API requests with an invalid name, they will receive a 400 error along with the error code CUSTOMER_NOT_FOUND, instead of an empty array. This approach allows you to gracefully handle errors and provide meaningful feedback to the client.


#### Validating API Parameters:
Apply validators with the validation filters to validate API parameters. Each validator has a corresponding filter with an is_ prefix, such as the is_integer filter for the integer validator.

To use these filters, chain them after your parameters:

```sql
-- orders.sql
SELECT * FROM
orders
WHERE order_id = {{ context.params.order_id | is_required | is_uuid }}
```
To set arguments for validators, use Python's keyword arguments syntax:

```sql
-- orders.sql
SELECT * FROM
orders
WHERE create_date = {{ context.params.create_date | is_date(format='YYYY-MM-DD') }}
AND price = {{ context.params.price | is_integer(min=0, max=1000000) }}
```
If a request doesn't meet the validator requirements, the API rejects the request and returns an error response. Here's an example:

```sql
-- orders.sql
SELECT * FROM
orders
where price = {{ context.params.price | is_integer(min=0, max=1000000) }}
```

If you send a request with GET<endpoint>/api/orders?price=1000001, the API returns an HTTP status code 400 along with the following message:

```json
{
  "code": "flapi.userError",
  "message": "The input parameter is invalid, it should be integer type",
}
```

This approach ensures that only valid requests are processed, providing better error handling and more informative feedback to clients.

##### Supported Validators
###### `is_required` - Required Validator
Makes the request parameter field required, using Joi for validation.

| Argument  | Required | Description                                                                                                  |
|-----------|----------|--------------------------------------------------------------------------------------------------------------|
| disallow  | false    | Array type. Specifies disallowed input values. If the parameter value appears in the disallow value, an error message is sent in the response. |

####### Example 1: Required Validator
```sql
-- orders.sql
SELECT * FROM
orders
WHERE order_id = {{ context.params.order_id | is_required }}
```

####### Example 2: Required Validator with Disallowed Values
```sql
-- orders.sql
SELECT * FROM
orders
WHERE order_id = {{ context.params.order_id | is_required(disallow=['']) }}
```

####### Error Response
When sending a request with `GET<endpoint>/api/dep_users?department=null`, it returns an HTTP status code 400 and the following message:
```json
{
  "code": "vulcan.userError",
  "message": "The input parameter is invalid, it should be required",
}
```

---

###### `is_uuid` - UUID Validator
Validates the UUID format for the request parameter.

| Argument | Required | Description                                          |
|----------|----------|------------------------------------------------------|
| version  | false    | String type. Specifies the UUID version (uuidv1, uuidv4, or uuidv5). |

####### Example 1: UUID Validator
```sql
-- orders.sql
SELECT * FROM
orders
WHERE order_id = {{ context.params.order_id | is_uuid }}
```

####### Example 2: UUID Validator with Version
```sql
-- orders.sql
SELECT * FROM
orders
WHERE order_id = {{ context.params.order_id | is_uuid(version='uuidv4') }}
```

---

###### `is_date` - Date Validator
Validates the date format for the request parameter.

| Argument | Required | Description                                                                                             |
|----------|----------|---------------------------------------------------------------------------------------------------------|
| format   | false    | String type. Specifies the date format, supporting ISO_8601 tokens (e.g., 'YYYYMMDD', 'YYYY-MM-DD', 'YYYY-MM-DD HH:mm'). |


####### Example 1: Date Validator
```sql
-- orders.sql
SELECT * FROM
orders
WHERE create_date = {{ context.params.create_date | is_date }}
```

####### Example 2: Date Validator with Format
```sql
-- orders.sql
SELECT * FROM
orders
WHERE create_date = {{ context.params.create_date | is_date(format='YYYY-MM-DD') }}
```

---

###### `is_string` - String Validator
Validates the string format for the request parameter.

| Argument | Required | Description                                                           |
|----------|----------|-----------------------------------------------------------------------|
| format   | false    | String type. Specifies the string format, supporting regular expressions. Uses Joi pattern for validation. |
| length   | false    | Number type. Specifies the exact length of the string.                |
| max      | false    | Number type. Specifies the maximum number of string characters.       |
| min      | false    | Number type. Specifies the minimum number of string characters.       |

####### Example 1: String Validator
```sql
-- orders.sql
SELECT * FROM
orders
WHERE order_id = {{ context.params.order_id | is_string }}
```

####### Example 2: String Validator with Format
```sql
-- orders.sql
SELECT * FROM
orders
WHERE order_id = {{ context.params.order_id | is_string(format='^[a-zA-Z0-9]{3,30}$') }}
```

####### Example 3: String Validator with Length
```sql
-- orders.sql
SELECT * FROM
orders
WHERE order_id = {{ context.params.order_id | is_string(length=10) }}
```

####### Example 4: String Validator with Min & Max
```sql
-- orders.sql
SELECT * FROM
orders
WHERE order_id = {{ context.params.order_id | is_string(min=3, max=30) }}
```

---

###### `is_integer` - Integer Validator
Validates the integer format for the request parameter.

| Argument | Required | Description                                      |
|----------|----------|--------------------------------------------------|
| max      | false    | Number type. Specifies the maximum integer value. |
| min      | false    | Number type. Specifies the minimum integer value. |
| greater  | false    | Number type. Specifies that the value must be greater than the limit. |
| less     | false    | Number type. Specifies that the value must be less than the limit.    |


####### Example 1: Integer Validator
```sql
-- orders.sql
SELECT * FROM
orders
WHERE price = {{ context.params.price | is_integer }}
```

####### Example 2: Integer Validator with Min & Max
```sql
-- orders.sql
SELECT * FROM
orders
WHERE price = {{ context.params.price | is_integer(min=0, max=1000000) }}
```

####### Example 3: Integer Validator with Greater & Less
```sql
-- orders.sql
SELECT * FROM
orders
WHERE price = {{ context.params.price | is_integer(greater=0, less=1000000) }}
```

---

###### `is_enum` - Enum Validator
Validates the enum format for the request parameter.

| Argument | Required | Description                                                                              |
|----------|----------|------------------------------------------------------------------------------------------|
| items    | true     | Array type. Specifies the whitelist. Must add at least one element to this array. Enum validator does not care about the data type, so both [1] and ["1"] allow the value 1 no matter if it is a string or number. |

####### Example 1: Enum Validator
```sql
-- orders.sql
SELECT * FROM
orders
WHERE status = {{ context.params.status | is_enum(items=['pending', 'paid', 'shipped']) }}
```


### API Configuration

#### Response Format:
The Response Format can configured to supported API response formats and set the preferred format based on the HTTP Header - Accept. You can customize the settings using the project configuration file (flapi.yaml by default) to meet your specific needs.

```yaml
response-format:
  enabled: true
  options:
    default: json
    formats:
      - json
      - csv
```

##### Response Format Options
| Name     | Default | Type    | Description                                                                 |
| -------- | ------- | ------- | --------------------------------------------------------------------------- |
| enabled  | true    | boolean | Enable or disable the response-format service. Set to `false` to stop it.  |

| Name     | Default | Type   | Description                                                                                          |
| -------- | ------- | ------ | ---------------------------------------------------------------------------------------------------- |
| default  | json    | string | The default format used when the request path doesn't specify a format type. Must be `json` or `csv`. |
| formats  | -       | list   | A list of supported response format types. Include the format type if you need to use it in the path. |

##### Examples
| Accept                                   | API URL Path        | options.default | options.formats  | response format type |
| ---------------------------------------- | ----------------- | --------------- | ---------------- | -------------------- |
| `application/json;q=0.9`                 | `/api/data`      | `json`          | `["json","csv"]` | json                 |
| `application/json;q=0.8, text/csv;q=0.9` | `/api/data`      | `json`          | `["json","csv"]` | csv                  |
| Not set                                  | `/api/data`      | `json`          | `["json","csv"]` | json                 |
| Not set                                  | `/api/data`      | `json`          | `["csv","json"]` | csv                  |
| Not set                                  | `/api/data`      | `json`          | Not set          | json                 |
| `application/json;q=0.9`                 | `/api/data.csv`  | `json`          | `["json","csv"]` | csv                  |
| `application/json;q=0.9, text/csv;q=0.9` | `/api/data.json` | `csv`           | `["csv"]`        | Error                |
| `application/json;q=0.9, text/csv;q=0.9` | `/api/data`      | Not set         | Not set          | json                 |

#### Pagination:
In some cases, we execute SQL queries to list all entities, which may result in a large amount of data. Instead of sending all the data to our clients, we can paginate the results and send only the necessary portion. This approach is a common design in modern API servers.

You can implement your own pagination strategies by modifying your SQL queries. For example, you can use parameters to implement a simple offset-based pagination:

```sql
SELECT * FROM users
LIMIT {{ context.params.limit }}
OFFSET {{ context.params.offset }}
```

flAPI provides several built-in pagination strategies. You can easily use them by configuring the settings in the vulcan.yaml, without the need to modify your SQL template.

| Mode                          | Description                                            |
| ----------------------------- | ------------------------------------------------------ |
| [offset](./pagination/offset) | Offset-based pagination e.g. `/path?offset=10&limit=100` |
| cursor                        | TBD                                                    |
| keyset                        | TBD                                                    |

Offset pagination is a simple pagination strategy that uses the limit parameter to select a specific number of results and the offset parameter to skip a certain number of results.

*Pros of Offset Pagination*
Random access: You can jump to any desired position by specifying the offset value. For example, you can fetch the 10001st row by setting offset=10000 and limit=1.

*Cons of Offset Pagination*
Performance impact: The database or warehouse needs to iterate through the skipped rows before retrieving the data. For instance, if you skip 10000 rows and fetch 1 row, it has to traverse all the 10000 rows before providing the result.

Set pagination.mode in the endpoint configuration to offset to enable this pagination strategy.

To fetch the first 10 rows: /api/customers?limit=10
To fetch rows 11 to 20: /api/customers?limit=10&offset=10

#### CORS:
CORS, or Cross-Origin Resource Sharing, is an important web security feature that allows servers to control which domains can access their resources. It mitigates potential security risks by restricting unauthorized access to server resources.

```yaml
cors:
  enabled: true
  options:
    allowed_origins:
      - "https://example.com"
    allowed_methods:
      - "GET"cors:
  enabled: true
  options:
    origin: 'http://google.com'
    allowMethods: 'GET,HEAD,PUT'
```

| Setting | Default | Type    | Description                          |
|---------|---------|---------|--------------------------------------|
| enabled | true    | boolean | Enable or disable the `cors` service |

| Option      | Default                         | Type                    | Description                                           |
|-------------|---------------------------------|-------------------------|-------------------------------------------------------|
| origin      | {request Origin header}         | string or Function(ctx) | Origin for `Access-Control-Allow-Origin`              |
| allowMethods | 'GET,HEAD,PUT,POST,DELETE,PATCH' | string            | Allowed methods in the string: `GET`, `HEAD`, `PUT`, `POST`, `DELETE`, and `PATCH` |
      

#### Rate Limiting:
Rate limiting is a crucial plugin for ensuring the stability and security of your API. By implementing rate limiting, you can prevent abuse and excessive requests to your server.

```yaml
rate-limit:
  enabled: true
  options:
    interval:
      min: 1
    max: 10000
```

| Setting  | Default | Type    | Description                                                 |
| -------- | ------- | ------- | ----------------------------------------------------------- |
| enabled  | true    | boolean | Enable or disable rate limit service (true to enable).     |

| Option   | Default | Type  | Description                                                            |
| -------- | ------- | ----- | ---------------------------------------------------------------------- |
| interval | null    | time  | Length of time request records are kept in memory (default: 1 minute). |
| max      | 60      | number| Maximum number of connections during `interval` before sending a HTTP 429.  |


#### Authentication:
To ensure that only authorized users access the API, it's essential to identify who sends the request. This process, called authentication, typically involves validating credentials such as tokens, cookies, or other forms of identification.

In flAPI, Authenticators handle this process, either by validating the credentials themselves or by querying third-party authentication providers. Once the authentication process is complete, additional user attributes are also added to the request for further processing. These attributes can include user names, departments, groups, statuses, and more, and they are used for authorization.

To enable authenticators, set auth.enabled to true in your flapi.yaml configuration file:

```yaml
auth:
  enabled: true
```
flAPI offers a range of built-in authenticators, each with its unique method of validation. You can find more information about how to enable and configure these authenticators in their respective documentation. Note that you can enable multiple authenticators if they validate different credentials.

Here is a list of built-in authenticators:

- HTTP Basic: Authenticate users via HTTP basic authentication. This method requires users to provide a username and password, which are transmitted as headers in each request.
- OAuth 2.0: Validate OAuth 2.0 tokens, which are used to authenticate and authorize requests.
- SAML: Validate SAML tokens, which are used to authenticate and authorize requests.
- JWT: Validate JSON Web Tokens (JWT), which are used to securely transmit information between parties.
- Custom: Use a custom authenticator to implement your own authentication logic.

##### HTTP Basic Authentication

With HTTP basic authentication, authenticates users via HTTP Basic Authentication.

```yaml
auth:
  enabled: true
  options:
    basic:
      # Read users and passwords from a text file.
      htpasswd-file:
        path: passwd.txt # Path to the password file.
        users: # (Optional) Add attributes for users
          - name: eason
            attr:
              department: engineer
```

You need to add a header `Authorization: basic base64(<username>:<password>)` when sending requests, for example, sending requests with username "ivan" and password "123".

##### OAuth 2.0
TODO

##### SAML
TODO

##### JWT
TODO

##### Custom
TODO

#### Authorization:
We manage user access to specific data sources by applying authorization policies to individual profiles associated with each data source. We use an attribute-based access control (ABAC) approach to control access based on user attributes provided by Authenticator. This allows for a clear and flexible way to control which users can access the data sources.

The configurations for each data source are defined in the profiles.yaml file. To configure authorization policies for each data source, you will need to set the allow property for each profile. The allow property can be a string, an array of strings, or an array of constraints. A constraint can have the following structure:

| Property   | Type           | Description                                                                                       |
|------------|----------------|---------------------------------------------------------------------------------------------------|
| name       | string         | Set a name constraint, with wildcard support. For example, `"admin"`, `"admin*"`, etc.                |
| attributes | Map<string, any> | Set an attributes constraint, with wildcard support for both keys and values. For example, `{"group": "admin*", "enabled": true}`. |


##### Example 1: Allow everyone to access the data source
```yaml title="profiles.yaml"
- name: pg
  type: pg
  allow: '*'
```

##### Example 2: Allow only users whose names match the pattern "admin*"
```yaml title="profiles.yaml"
- name: pg
  type: pg
  allow: 'admin*'
```

##### Example 3: Allow only users who have the attribute "group" set to "admin"
```yaml title="profiles.yaml"
- name: pg
  type: pg
  allow:
    - attributes:
        group: admin
```

##### Example 4: Allow only users who have the attribute "group" set to "admin" and the attribute "enabled" set to true
```yaml title="profiles.yaml"
- name: pg
  type: pg
  allow:
    - name: 'admin*'
      attributes:
        group: 'admin*'
        enabled: 'true'
```

##### Set the allowed profiles to each template

For every SQL template, we need to tell flAPI what profiles they could use by adding `profiles` property on the schema. 
From top to bottom, users use the first qualified profile. If users can't use any of them, 403 error will be thrown.

```yaml
urlPath: /customer
profiles:
  - pg-admin
  - pg-non-admin
```

#### Column Level Security
Column Level Security (CLS) is a data security measure that provides fine-grained control over access to specific columns of a database table. With CLS, database administrators can restrict access to sensitive data by allowing only authorized users or roles to access specific columns. This helps to protect against data leaks or unauthorized access to sensitive information.

In the flAPI system, CLS can be achieved by utilizing if-else conditions in conjunction with user attributes, which are accessible through {{ context.user.attr }} in flAPI. For instance, to limit access to the salary column in the employees table, you may use the following query:

```sql
SELECT
  employee_id,
  first_name,
  last_name,
  {% if context.user.attr.department == 'intern' %}
    NULL AS salary
  {% else %}
    salary
  {% endif %}
FROM
  employees;
```

#### Dynamic Data Masking
Dynamic Data Masking is a Column-level Security feature that uses masking tag to selectively mask plain-text data in a query result set. It can be used to hide sensitive data in the result set of a query over designated fields, while the data in the database is not changed.

##### Using `masking` tag
You can use the `masking` tag with `{% ... %}`.

```sql
{% masking <column-name> <masking-function> %}
```

  Where:
* `<column-name>` is the name of the column to mask.
* `<masking-function>` is the masking function to use. Currently, VulcanSQL supports `partial` masking function.

##### Masking Data With `partial` Function

```js
partial(prefixTotal, padding, suffixTotal);
```

* `prefixTotal` is the number of characters to display at the beginning of the string.
* `padding` is the custom string to use for masking.
* `suffixTotal` is the number of characters to display at the end of the string.

Here is an example of how to use the `partial` function with the `masking` tag. Assuming we have a `users` table with an `id` column, and we want to partially mask the first two and last two characters of each `id` value:

```sql
SELECT
  {% masking id partial(2, 'xxxxxxx', 2) %} as id,
  name
FROM users;
```

This will return a result where the first two and last two characters of each `id` value are replaced with `x`:

| id          | name    |
| ----------- | ------- |
| ABxxxxxxxCD | Ivan    |
| EFxxxxxxxGH | William |

#### Row Level Security
Row Level Security (RLS) is a feature that allows you to control access to individual rows in a table based on the characteristics of the user who is accessing the data. With RLS, you can define a set of rules that determine which users can see which rows in a table.

To implement RLS, user attributes can be conveniently included in the WHERE clause to constrain the data a user can access. For instance, if you wish to permit a user to access only data associated with their department, the following query can be utilized:

```sql
SELECT * FROM employees
WHERE department = {{ context.user.attr.department }}
```

Moreover, you can generate an error if a user attempts to access data they are not authorized to view:

For example, to prevent interns from accessing data after 2023, the following query can be employed:

```sql
{% if context.user.attr.department == 'intern' and context.params.year < 2023 %}
    {% error "OUT_OF_LIMITED" %}
{% endif %}
```

#### Monitoring and Logging:
The Access Log allows you to keep track of incoming requests and monitor the activity on your API server. With the Access Log, you can easily identify and analyze traffic patterns, detect issues, and optimize the performance of your API.

```yaml
access-log:
  enabled: true
  options:
    level: info
    displayRequestId: true
    displayFunctionName: true
    displayFilePath: hideNodeModulesOnly
```

| Name     | Default | Type     | Description                                                                 |
|----------|---------|----------|-----------------------------------------------------------------------------|
| enabled  | true    | boolean  | Determines whether to enable the `access-log` service. Set to `false` to disable the service. |

| Name               | Default               | Type    | Description                                                                                 | Options                                             |
|--------------------|-----------------------|---------|---------------------------------------------------------------------------------------------|-----------------------------------------------------|
| level              | debug                 | string  | Sets the log level                                                                         | silly, trace, debug, info, warn, error, or fatal   |
| displayRequestId   | false                 | boolean | Determines whether to display the request ID in logs                                       |                                                     |
| displayFunctionName| false                 | boolean | Determines whether to display the function name in logs                                    |                                                     |
| displayFilePath    | hidden                | string  | Sets how the file path is displayed in logs                                                | hidden, displayAll, or hideNodeModulesOnly         |


### Deployment

Once your development and testing phases are complete, it's time to deploy to production servers. The cool thin is that flAPI is just a single executable and a bunch of configration files. It is further possible to run it as a Docker container or as a AWS Lambda function. The connection to the data source is made via DuckDB. Therefore the connection configuration is specific to the datastore and the way it is exposed to DuckDB. 



## Steps to generate the new project

- Please setup a CMake project
- Add the neccessary dependencies via VCPKG to the project
- Source files should go into src/, header into src/include/
- Add a CMake target for the flapi executable
- Add also an extensive test suite in /test/ for the different components, use the catch2 framework for that.
- Add a CMake target for the test suite
- Create a vscode launch.json and task.json to compile and debug the project, as well as the tests