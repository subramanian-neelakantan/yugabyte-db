// Copyright (c) YugaByte, Inc.

import React, { FC, useEffect } from 'react';
import { useDispatch, useSelector } from 'react-redux';

import {
  queryMetrics,
  queryMetricsSuccess,
  queryMetricsFailure,
  currentTabSelected
} from '../../../actions/graph';
import { getTabContent } from '../../../utils/GraphUtils';
import { isNonEmptyArray, isNonEmptyString } from '../../../utils/ObjectUtils';
import { MetricsData, GraphFilter, MetricQueryParams } from '../../../redesign/helpers/dtos';
import {
  MetricMeasure,
  MetricConsts,
  SplitType,
  MetricTypes,
  NodeAggregation,
  NodeType
} from '../../metrics/constants';

export const GraphTab: FC<MetricsData> = ({
  type,
  metricsKey,
  nodePrefixes,
  selectedUniverse,
  title,
  tableName
}) => {
  let tabContent = null;
  const insecureLoginToken = useSelector((state: any) => state.customer.INSECURE_apiToken);
  const { currentUser } = useSelector((state: any) => state.customer);
  const graph = useSelector((state: any) => state.graph);
  const {
    startMoment,
    endMoment,
    nodeName,
    currentSelectedNodeType,
    nodePrefix,
    currentSelectedRegion,
    metricMeasure,
    outlierType,
    outlierNumNodes,
    selectedRegionClusterUUID,
    selectedRegionCode,
    selectedZoneName
  }: GraphFilter = graph.graphFilter;
  const dispatch: any = useDispatch();

  const queryMetricsType = () => {
    const metricsWithSettings = metricsKey.map((metricKey) => {
      const settings: any = {
        metric: metricKey
      };
      if (metricMeasure === MetricMeasure.OUTLIER) {
        settings.nodeAggregation = NodeAggregation.AVERAGE;
        settings.splitMode = outlierType;
        settings.splitCount = outlierNumNodes;
        settings.splitType = SplitType.NODE;
        // Top K tables section will be displayed only in case of Metrics Measure "Overall"
      } else if (type === MetricTypes.OUTLIER_TABLES) {
        settings.splitMode = outlierType;
        settings.splitCount = outlierNumNodes;
        settings.returnAggregatedValue = false;
        settings.splitType = SplitType.TABLE;
      }
      return settings;
    });

    const params: any = {
      metricsWithSettings: metricsWithSettings,
      start: startMoment.format('X'),
      end: endMoment.format('X')
    };
    if (isNonEmptyString(nodePrefix) && nodePrefix !== MetricConsts.ALL) {
      params.nodePrefix = nodePrefix;
    }

    // In case of universe metrics , nodePrefix comes from component itself
    if (isNonEmptyArray(nodePrefixes)) {
      params.nodePrefix = nodePrefixes[0];
    }

    // Top K tables section should not have the below query params
    if (
      isNonEmptyString(nodeName) &&
      nodeName !== MetricConsts.ALL &&
      nodeName !== MetricConsts.TOP
    ) {
      params.nodeNames = [nodeName];
    }
    // If specific region or cluster is selected from region dropdown, pass clusterUUID and region code
    if (isNonEmptyString(selectedRegionClusterUUID)) {
      params.clusterUuids = [selectedRegionClusterUUID];
    }

    if (isNonEmptyString(currentSelectedNodeType) && currentSelectedNodeType !== NodeType.ALL) {
      params.serverType = currentSelectedNodeType?.toUpperCase();
    }

    if (isNonEmptyString(selectedZoneName)) {
      params.availabilityZones = [selectedZoneName];
    }

    if (isNonEmptyString(selectedRegionCode)) {
      params.regionCodes = [selectedRegionCode];
    }

    if (isNonEmptyString(tableName)) {
      params.tableName = tableName;
    }

    queryMetricsVaues(params, type);
  };

  const queryMetricsVaues = (params: MetricQueryParams, type: string) => {
    dispatch(queryMetrics(params)).then((response: any) => {
      if (!response.error) {
        dispatch(queryMetricsSuccess(response.payload, type));
      } else {
        dispatch(queryMetricsFailure(response.payload, type));
      }
    });
  };

  const setSelectedTabName = (type: string) => {
    dispatch(currentTabSelected(type));
  };

  useEffect(() => {
    setSelectedTabName(type);
    queryMetricsType();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [
    nodeName,
    nodePrefix,
    startMoment,
    endMoment,
    currentSelectedRegion,
    metricMeasure,
    outlierType,
    outlierNumNodes
  ]);

  tabContent = getTabContent(
    graph,
    selectedUniverse,
    type,
    metricsKey,
    title,
    currentUser,
    insecureLoginToken
  );

  return <>{tabContent}</>;
};
