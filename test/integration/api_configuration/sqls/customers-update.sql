-- Update customer (UPDATE)
-- Note: This uses an in-memory table for testing purposes

CREATE TABLE IF NOT EXISTS test_customers (
    id INTEGER PRIMARY KEY,
    name VARCHAR(100),
    segment VARCHAR(50),
    balance DOUBLE,
    comment TEXT,
    email VARCHAR(100),
    registration_date DATE,
    last_login_time TIME,
    uuid VARCHAR(36)
);

UPDATE test_customers
SET
    name = {{#params.name}}'{{{ params.name }}}'{{/params.name}}{{^params.name}}name{{/params.name}},
    segment = {{#params.segment}}'{{{ params.segment }}}'{{/params.segment}}{{^params.segment}}segment{{/params.segment}},
    balance = {{#params.balance}}{{{ params.balance }}}{{/params.balance}}{{^params.balance}}balance{{/params.balance}},
    comment = {{#params.comment}}'{{{ params.comment }}}'{{/params.comment}}{{^params.comment}}comment{{/params.comment}},
    email = {{#params.email}}'{{{ params.email }}}'{{/params.email}}{{^params.email}}email{{/params.email}},
    registration_date = {{#params.registration_date}}DATE '{{{ params.registration_date }}}'{{/params.registration_date}}{{^params.registration_date}}registration_date{{/params.registration_date}},
    last_login_time = {{#params.last_login_time}}TIME '{{{ params.last_login_time }}}'{{/params.last_login_time}}{{^params.last_login_time}}last_login_time{{/params.last_login_time}},
    uuid = {{#params.uuid}}'{{{ params.uuid }}}'{{/params.uuid}}{{^params.uuid}}uuid{{/params.uuid}}
WHERE id = {{{ params.customer_id }}};

SELECT * FROM test_customers WHERE id = {{{ params.customer_id }}};
