-- Invalid input
SELECT polar_feature_utils.polar_advisor_feature_name(-1);
SELECT polar_feature_utils.polar_advisor_feature_name(10000);
SELECT polar_feature_utils.polar_advisor_feature_value(-1);
SELECT polar_feature_utils.polar_advisor_feature_value(10000);

-- We cannot show the value because the value is volatile
SELECT id, name FROM polar_feature_utils.polar_unique_feature_usage ORDER BY id;
