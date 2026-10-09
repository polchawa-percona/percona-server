# Reset the performance_schema.setup_consumers table

UPDATE performance_schema.setup_consumers SET enabled = 'YES';
UPDATE performance_schema.setup_consumers SET enabled = 'NO' WHERE name =
    'latch_source_summary';
