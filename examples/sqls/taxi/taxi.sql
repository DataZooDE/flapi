select filename, VendorID, count(*) 
from read_parquet('{{{conn.path}}}/*.parquet', filename=true) 
group by 1, 2
