set client_min_messages='warning';
CREATE EXTENSION if not exists polar_px;
SELECT polar_px_workerid();
reset client_min_messages;
