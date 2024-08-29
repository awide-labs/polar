CREATE EXTENSION test_slru;

-- See README for explanation of arguments:
SELECT test_slru();

SELECT test_slru_hash_index(10, 256, 10000);
SELECT test_slru_slot_size_config();
