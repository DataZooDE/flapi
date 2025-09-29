CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT 
    FUNCNAME as function_name,
    GROUPNAME as function_group_name,
    STEXT as function_description
FROM sap_rfc_show_function()