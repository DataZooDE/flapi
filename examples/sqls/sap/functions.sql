select 
    FUNCNAME as function_name,
    GROUPNAME as function_group_name,
    STEXT as function_description
from sap_rfc_show_function()